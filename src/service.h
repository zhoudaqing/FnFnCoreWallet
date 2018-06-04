// Copyright (c) 2017-2018 The Multiverse developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef  MULTIVERSE_SERVICE_H
#define  MULTIVERSE_SERVICE_H

#include "mvbase.h"

namespace multiverse
{

class CService : public IService
{
public:
    CService();
    ~CService();
    /* Notify */
    void NotifyWorldLineUpdate(const CWorldLineUpdate& update);
    /* System */
    void Shutdown();
    /* Network */
    int GetPeerCount();
    bool AddNode(const walleve::CNetHost& node);
    void RemoveNode(const walleve::CNetHost& node);
    /* Worldline & Tx Pool*/
    int  GetForkCount();
    bool GetForkGenealogy(const uint256& hashFork,std::vector<std::pair<uint256,int> >& vAncestry,
                                                  std::vector<std::pair<int,uint256> >& vSubline);
    bool GetBlockLocation(const uint256& hashBlock,uint256& hashFork,int& nHeight);
    int  GetBlockCount(const uint256& hashFork);
    bool GetBlockHash(const uint256& hashFork,int nHeight,uint256& hashBlock);
    bool GetBlock(const uint256& hashBlock,CBlock& block,uint256& hashFork,int& nHeight);
    void GetTxPool(const uint256& hashFork,std::vector<uint256>& vTxPool);
    bool GetTransaction(const uint256& txid,CTransaction& tx,std::vector<uint256>& vInBlock);
    MvErr SendTransaction(CTransaction& tx);
    /* Wallet */
    bool HaveKey(const crypto::CPubKey& pubkey);
    void GetPubKeys(std::set<crypto::CPubKey>& setPubKey);
    bool GetKeyStatus(const crypto::CPubKey& pubkey,int& nVersion,bool& fLocked,int64& nAutoLockTime);
    bool MakeNewKey(const crypto::CCryptoString& strPassphrase,crypto::CPubKey& pubkey);
    bool AddKey(const crypto::CKey& key);
    bool ImportKey(const std::vector<unsigned char>& vchKey,crypto::CPubKey& pubkey);
    bool ExportKey(const crypto::CPubKey& pubkey,std::vector<unsigned char>& vchKey);
    bool EncryptKey(const crypto::CPubKey& pubkey,const crypto::CCryptoString& strPassphrase,
                                                  const crypto::CCryptoString& strCurrentPassphrase);
    bool Lock(const crypto::CPubKey& pubkey);
    bool Unlock(const crypto::CPubKey& pubkey,const crypto::CCryptoString& strPassphrase,int64 nTimeout);
    bool SignSignature(const crypto::CPubKey& pubkey,const uint256& hash,std::vector<unsigned char>& vchSig);
    /* Util */
protected:
    bool WalleveHandleInitialize();
    void WalleveHandleDeinitialize();
    bool WalleveHandleInvoke();
    void WalleveHandleHalt();
    
protected:
    ICoreProtocol* pCoreProtocol;
    IWorldLine* pWorldLine;
    IWallet* pWallet;
    mutable boost::shared_mutex rwForkStatus;
    std::map<uint256,CForkStatus> mapForkStatus;
};

} // namespace multiverse

#endif //MULTIVERSE_SERVICE_H
