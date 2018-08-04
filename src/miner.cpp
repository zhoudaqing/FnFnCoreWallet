// Copyright (c) 2017-2018 The Multiverse developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "miner.h"

#include "json/json_spirit_reader_template.h"
#include "json/json_spirit_writer_template.h"
#include "json/json_spirit_utils.h"

using namespace std;
using namespace multiverse;
using namespace walleve;
using namespace json_spirit;
using boost::asio::ip::tcp;

extern void MvShutdown();

///////////////////////////////
// CMiner

CMiner::CMiner(const vector<string>& vArgsIn)
: IIOModule("miner"),
  thrFetcher("fetcher",boost::bind(&CMiner::LaunchFetcher,this)),
  thrMiner("miner",boost::bind(&CMiner::LaunchMiner,this))
{
    nNonceGetWork = 1;
    nNonceSubmitWork = 2;
    pHttpGet = NULL;
    if (vArgsIn.size() >= 3)
    {
        strAddrSpent = vArgsIn[1];
        strMintKey = vArgsIn[2];
    } 
}

CMiner::~CMiner()
{
}

bool CMiner::WalleveHandleInitialize()
{
    if (!WalleveGetObject("httpget",pHttpGet))
    {
        cerr << "Failed to request httpget\n";
        return false;
    }
    return true;
}

void CMiner::WalleveHandleDeinitialize()
{
    pHttpGet = NULL;
}

bool CMiner::WalleveHandleInvoke()
{
    if (strAddrSpent.empty())
    {
        cerr << "Invalid spent address\n";
        return false;
    }
    if (strMintKey.empty())
    {
        cerr << "Invalid mint key\n";
        return false;
    }
    if (!WalleveThreadDelayStart(thrFetcher))
    {
        return false;
    }
    if (!WalleveThreadDelayStart(thrMiner))
    {
        return false;
    }
    nMinerStatus = MINER_RUN;
    return IIOModule::WalleveHandleInvoke();
}

void CMiner::WalleveHandleHalt()
{
    IIOModule::WalleveHandleHalt();

    {
        boost::unique_lock<boost::mutex> lock(mutex);
        nMinerStatus = MINER_EXIT;
    }
    condFetcher.notify_all();
    condMiner.notify_all();
   
    if (thrFetcher.IsRunning())
    {
        CancelRPC();
        thrFetcher.Interrupt();
    }
    thrFetcher.Exit();

    if (thrMiner.IsRunning())
    {
        thrMiner.Interrupt();
    }
    thrMiner.Exit();
}

const CMvRPCConfig * CMiner::WalleveConfig()
{
    return dynamic_cast<const CMvRPCConfig *>(IWalleveBase::WalleveConfig());
}

bool CMiner::HandleEvent(CWalleveEventHttpGetRsp& event)
{
    try
    {
        CWalleveHttpRsp& rsp = event.data;

        if (rsp.nStatusCode < 0)
        {
            
            const char * strErr[] = {"","connect failed","invalid nonce","activate failed",
                                     "disconnected","no response","resolve failed",
                                     "internal failure","aborted"};
            throw runtime_error(rsp.nStatusCode >= HTTPGET_ABORTED ? 
                                strErr[-rsp.nStatusCode] : "unknown error");
        }
        if (rsp.nStatusCode == 401)
        {
            throw runtime_error("incorrect rpcuser or rpcpassword (authorization failed)");
        }
        else if (rsp.nStatusCode > 400 && rsp.nStatusCode != 404 && rsp.nStatusCode != 500)
        {
            ostringstream oss;
            oss << "server returned HTTP error " << rsp.nStatusCode;
            throw runtime_error(oss.str());
        }
        else if (rsp.strContent.empty())
        {
            throw runtime_error("no response from server");
        }

        // Parse reply
        Value valReply;
        if (!read_string(rsp.strContent, valReply))
        {
            throw runtime_error("couldn't parse reply from server");
        }
        const Object& reply = valReply.get_obj();
        if (reply.empty())
        {
            throw runtime_error("expected reply to have result, error and id properties");
        }

        const Value& result = find_value(reply, "result");
        const Value& error  = find_value(reply, "error");

        if (error.type() == null_type)
        {
            if (event.nNonce == nNonceGetWork) 
            {
                if (result.type() == bool_type)
                {
                    if (!result.get_bool())
                    {
                        {
                            boost::unique_lock<boost::mutex> lock(mutex);

                            workCurrent.SetNull();
                            nMinerStatus = MINER_HOLD;
                        }
                        condMiner.notify_all(); 
                    }
                }
                else if (result.type() == obj_type)
                {
                    const Object& obj = result.get_obj();
                    const Value& prevhash = find_value(obj,"prevblockhash");
                    const Value& prevtime = find_value(obj,"prevblocktime");
                    const Value& algo = find_value(obj,"algo");
                    const Value& bits = find_value(obj,"bits");
                    const Value& data = find_value(obj,"data");
                    {
                        boost::unique_lock<boost::mutex> lock(mutex);
 
                        workCurrent.hashPrev.SetHex(prevhash.get_str());
                        workCurrent.nPrevTime = prevtime.get_int64();
                        workCurrent.nAlgo = algo.get_int();
                        workCurrent.nBits = algo.get_int();
                        workCurrent.vchWorkData = ParseHexString(data.get_str());

                        nMinerStatus = MINER_RESET;
                    }
                    condMiner.notify_all(); 
                }
            } 
            else if (event.nNonce == nNonceSubmitWork)
            {
                if (result.type() == str_type)
                {
                    cout << "Submited new block : " << result.get_str() << "\n";
                }
            }
            return true;
        }
        else
        {
            // Error
            cerr << "error: " << write_string(error, false) << "\n";
        }
    }
    catch (std::exception& e)
    {
        cerr << "error: " << e.what() << "\n";
    }
    catch (...)
    {
        cerr << "unknown exception\n";
    }
    return true;
}

bool CMiner::SendRequest(uint64 nNonce,Object& jsonReq)
{
    CWalleveEventHttpGet eventHttpGet(nNonce);
    CWalleveHttpGet& httpGet = eventHttpGet.data;
    httpGet.strIOModule = WalleveGetOwnKey();
    httpGet.nTimeout = WalleveConfig()->nRPCConnectTimeout;;
    if (WalleveConfig()->fRPCSSLEnable)
    {
        httpGet.strProtocol = "https";
        httpGet.fVerifyPeer = true;
        httpGet.strPathCA   = WalleveConfig()->strRPCCAFile;
        httpGet.strPathCert = WalleveConfig()->strRPCCertFile;
        httpGet.strPathPK   = WalleveConfig()->strRPCPKFile;
    }
    else
    {
        httpGet.strProtocol = "http";
    }

    CNetHost host(WalleveConfig()->strRPCConnect,WalleveConfig()->nRPCPort);
    httpGet.mapHeader["host"] = host.ToString();
    httpGet.mapHeader["url"] = string("/");
    httpGet.mapHeader["method"] = "POST";
    httpGet.mapHeader["accept"] = "application/json";
    httpGet.mapHeader["content-type"] = "application/json";
    httpGet.mapHeader["user-agent"] = string("multivers-json-rpc/");
    httpGet.mapHeader["connection"] = "Keep-Alive";
    if (!WalleveConfig()->strRPCPass.empty() || !WalleveConfig()->strRPCUser.empty())
    {
        string strAuth;
        CHttpUtil().Base64Encode(WalleveConfig()->strRPCUser + ":" + WalleveConfig()->strRPCPass,strAuth);
        httpGet.mapHeader["authorization"] = string("Basic ") + strAuth;
    }

    httpGet.strContent = write_string(Value(jsonReq), false) + "\n";
    
    return pHttpGet->DispatchEvent(&eventHttpGet);
}

bool CMiner::GetWork()
{
    Array params;
    try
    {
    //    nNonceGetWork += 2;
        params.push_back(workCurrent.hashPrev.GetHex());
        Object request;
        request.push_back(Pair("method", "getwork"));
        request.push_back(Pair("params", params));
        request.push_back(Pair("id", (int)nNonceGetWork));
        return SendRequest(nNonceGetWork,request);
    }
    catch (...)
    {
        cerr << "getwork exception\n";
    }
    return false;
}

bool CMiner::SubmitWork(const vector<unsigned char>& vchWorkData)
{
    Array params;
    try
    {
    //    nNonceSubmitWork += 2;

        params.push_back(ToHexString(vchWorkData));
        params.push_back(strAddrSpent);
        params.push_back(strMintKey);
        Object request;
        request.push_back(Pair("method", "submitwork"));
        request.push_back(Pair("params", params));
        request.push_back(Pair("id", (int)nNonceSubmitWork));
        return SendRequest(nNonceSubmitWork,request);
    }
    catch (...)
    {
        cerr << "submitwork exception\n";
    }
    return false;
}

void CMiner::CancelRPC()
{
    if (pHttpGet)
    {
        CWalleveEventHttpAbort eventAbort(0);
        CWalleveHttpAbort& httpAbort = eventAbort.data;
        httpAbort.strIOModule = WalleveGetOwnKey();
        httpAbort.vNonce.push_back(nNonceGetWork);
        httpAbort.vNonce.push_back(nNonceSubmitWork);
        pHttpGet->DispatchEvent(&eventAbort);
    }
}

uint256 CMiner::GetHashTarget(const CMinerWork& work,int64 nTime)
{
    int64 nPrevTime = work.nPrevTime;
    int nBits = work.nBits;

    if (nTime - nPrevTime < BLOCK_TARGET_SPACING)
    {
        return (nBits + 1);
    }

    nBits -= (nTime - nPrevTime - BLOCK_TARGET_SPACING) / PROOF_OF_WORK_DECAY_STEP;
    if (nBits < 16)
    {
        nBits = 16;
    } 
    return ((~uint256(0) >> nBits));
}

void CMiner::LaunchFetcher()
{
    while (nMinerStatus != MINER_EXIT) 
    {
        if (!GetWork())
        {
            cerr << "Failed to getwork\n";
        }
        boost::system_time const timeout = boost::get_system_time() + boost::posix_time::seconds(30);
        {
            boost::unique_lock<boost::mutex> lock(mutex);
            while(nMinerStatus != MINER_EXIT)
            {
                if (!condFetcher.timed_wait(lock,timeout) || nMinerStatus == MINER_HOLD)
                {
                    break;
                }
            }
        }
    }    
}

void CMiner::LaunchMiner()
{
    while (nMinerStatus != MINER_EXIT) 
    {
        CMinerWork work; 
        {
            boost::unique_lock<boost::mutex> lock(mutex);
            while (nMinerStatus == MINER_HOLD)
            {
                condMiner.wait(lock);
            }
            if (nMinerStatus == MINER_EXIT)
            {
                break;
            }
            if (workCurrent.IsNull() || workCurrent.nAlgo != POWA_BLAKE512)
            {
                nMinerStatus = MINER_HOLD;
                continue;
            }
            work = workCurrent;
            nMinerStatus = MINER_RUN;
        }

        uint32& nTime = *((uint32*)&work.vchWorkData[4]);
        uint256& nNonce = *((uint256*)&work.vchWorkData[work.vchWorkData.size() - 32]);

        if (work.nAlgo == POWA_BLAKE512)
        {
            cout << "Get blake512 work,prev block hash : " << work.hashPrev.GetHex() << "\n";
            uint256 hashTarget = GetHashTarget(work,nTime); 
            while (nMinerStatus == MINER_RUN)
            {
                int64 t = GetTime();
                if (t > nTime)
                {
                    nTime = t;
                    hashTarget = GetHashTarget(work,t);
                }
                for (int i = 0; i < 100 * 1024;i++,nNonce++)
                {
                    uint256 hash = crypto::CryptoHash(&work.vchWorkData[0],work.vchWorkData.size());
                    if (hash <= hashTarget)
                    {
                        cout << "Proof-of-work found\n hash : " << hash.GetHex() << "\ntarget : " << hashTarget.GetHex() << "\n";
                        if (!SubmitWork(work.vchWorkData))
                        {
                            cerr << "Failed to submit work\n";
                        }
                        boost::unique_lock<boost::mutex> lock(mutex);
                        if (nMinerStatus == MINER_RUN)
                        {
                            nMinerStatus = MINER_HOLD;
                        }
                        break;
                    }
                }
            }
            condFetcher.notify_all();
        }
    }
}
