// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NET_H
#define BITCOIN_NET_H

#include "addrdb.h"
#include "addrman.h"
#include "bloom.h"
#include "compat.h"
#include "limitedmap.h"
#include "netbase.h"
#include "protocol.h"
#include "random.h"
#include "streams.h"
#include "sync.h"
#include "uint256.h"
#include "util.h"

#include <atomic>
#include <deque>
#include <stdint.h>
#include <memory>

#ifndef WIN32
#include <arpa/inet.h>
#endif

#include <boost/filesystem/path.hpp>
#include <boost/foreach.hpp>
#include <boost/signals2/signal.hpp>

class CAddrMan;
class CScheduler;
class CNode;

namespace boost {
    class thread_group;
} // namespace boost

/** Time between pings automatically sent out for latency probing and keepalive (in seconds). */
static const int PING_INTERVAL = 2 * 60;
/** Time after which to disconnect, after waiting for a ping response (or inactivity). */
static const int TIMEOUT_INTERVAL = 20 * 60;
/** Minimum time between warnings printed to log. */
static const int WARNING_INTERVAL = 10 * 60;
/** Run the feeler connection loop once every 2 minutes or 120 seconds. **/
static const int FEELER_INTERVAL = 120;
/** The maximum number of entries in an 'inv' protocol message */
static const unsigned int MAX_INV_SZ = 50000;
/** The maximum number of new addresses to accumulate before announcing. */
static const unsigned int MAX_ADDR_TO_SEND = 1000;
/** Maximum length of incoming protocol messages (no message over 2 MiB is currently acceptable). */
static const unsigned int MAX_PROTOCOL_MESSAGE_LENGTH = 2 * 1024 * 1024;
/** Maximum length of strSubVer in `version` message */
static const unsigned int MAX_SUBVERSION_LENGTH = 256;
/** -listen default */
static const bool DEFAULT_LISTEN = true;
/** -upnp default */
#ifdef USE_UPNP
static const bool DEFAULT_UPNP = USE_UPNP;
#else
static const bool DEFAULT_UPNP = false;
#endif
/** The maximum number of entries in mapAskFor */
static const size_t MAPASKFOR_MAX_SZ = MAX_INV_SZ;
/** The maximum number of entries in setAskFor (larger due to getdata latency)*/
static const size_t SETASKFOR_MAX_SZ = 2 * MAX_INV_SZ;
/** The maximum number of peer connections to maintain. */
static const unsigned int DEFAULT_MAX_PEER_CONNECTIONS = 125;
/** The default for -maxuploadtarget. 0 = Unlimited */
static const uint64_t DEFAULT_MAX_UPLOAD_TARGET = 0;
/** Default for blocks only*/
static const bool DEFAULT_BLOCKSONLY = false;

static const bool DEFAULT_FORCEDNSSEED = false;
static const size_t DEFAULT_MAXRECEIVEBUFFER = 5 * 1000;
static const size_t DEFAULT_MAXSENDBUFFER    = 1 * 1000;

static const ServiceFlags REQUIRED_SERVICES = NODE_NETWORK;

// NOTE: When adjusting this, update rpcnet:setban's help ("24h")
static const unsigned int DEFAULT_MISBEHAVING_BANTIME = 60 * 60 * 24;  // Default 24-hour ban

unsigned int ReceiveFloodSize();
unsigned int SendBufferSize();

typedef int NodeId;

struct AddedNodeInfo
{
    std::string strAddedNode;
    CService resolvedAddress;
    bool fConnected;
    bool fInbound;
};

class CTransaction;
class CNodeStats;
class CConnman
{
public:

    enum NumConnections {
        CONNECTIONS_NONE = 0,
        CONNECTIONS_IN = (1U << 0),
        CONNECTIONS_OUT = (1U << 1),
        CONNECTIONS_ALL = (CONNECTIONS_IN | CONNECTIONS_OUT),
    };

    CConnman();
    ~CConnman();
    bool Start(boost::thread_group& threadGroup, CScheduler& scheduler, std::string& strNodeError);
    void Stop();
    bool BindListenPort(const CService &bindAddr, std::string& strError, bool fWhitelisted = false);
    bool OpenNetworkConnection(const CAddress& addrConnect, CSemaphoreGrant *grantOutbound = NULL, const char *strDest = NULL, bool fOneShot = false, bool fFeeler = false);
    bool CheckIncomingNonce(uint64_t nonce);

    // fConnectToMasternode should be 'true' only if you want this node to allow to connect to itself
    // and/or you want it to be disconnected on CMasternodeMan::ProcessMasternodeConnections()
    // Unfortunately, can't make this method private like in Bitcoin,
    // because it's used in many Dash-specific places (masternode, privatesend).
    CNode* ConnectNode(CAddress addrConnect, const char *pszDest = NULL, bool fConnectToMasternode = false);

    bool ForNode(NodeId id, std::function<bool(CNode* pnode)> func);
    bool ForNode(const CService& addr, std::function<bool(CNode* pnode)> func);
    bool ForEachNode(std::function<bool(CNode* pnode)> func);
    bool ForEachNode(std::function<bool(const CNode* pnode)> func) const;
    bool ForEachNodeThen(std::function<bool(CNode* pnode)> pre, std::function<void()> post);
    bool ForEachNodeThen(std::function<bool(const CNode* pnode)> pre, std::function<void()> post) const;

    std::vector<CNode*> CopyNodeVector();
    void ReleaseNodeVector(const std::vector<CNode*>& vecNodes);

    void RelayTransaction(const CTransaction& tx);
    void RelayTransaction(const CTransaction& tx, const CDataStream& ss);
    void RelayInv(CInv &inv, const int minProtoVersion = MIN_PEER_PROTO_VERSION);

    // Addrman functions
    size_t GetAddressCount() const;
    void SetServices(const CService &addr, ServiceFlags nServices);
    void MarkAddressGood(const CAddress& addr);
    void AddNewAddress(const CAddress& addr, const CAddress& addrFrom, int64_t nTimePenalty = 0);
    void AddNewAddresses(const std::vector<CAddress>& vAddr, const CAddress& addrFrom, int64_t nTimePenalty = 0);
    std::vector<CAddress> GetAddresses();
    void AddressCurrentlyConnected(const CService& addr);

    // Denial-of-service detection/prevention
    // The idea is to detect peers that are behaving
    // badly and disconnect/ban them, but do it in a
    // one-coding-mistake-won't-shatter-the-entire-network
    // way.
    // IMPORTANT:  There should be nothing I can give a
    // node that it will forward on that will make that
    // node's peers drop it. If there is, an attacker
    // can isolate a node and/or try to split the network.
    // Dropping a node for sending stuff that is invalid
    // now but might be valid in a later version is also
    // dangerous, because it can cause a network split
    // between nodes running old code and nodes running
    // new code.
    void Ban(const CNetAddr& netAddr, const BanReason& reason, int64_t bantimeoffset = 0, bool sinceUnixEpoch = false);
    void Ban(const CSubNet& subNet, const BanReason& reason, int64_t bantimeoffset = 0, bool sinceUnixEpoch = false);
    void ClearBanned(); // needed for unit testing
    bool IsBanned(CNetAddr ip);
    bool IsBanned(CSubNet subnet);
    bool Unban(const CNetAddr &ip);
    bool Unban(const CSubNet &ip);
    void GetBanned(banmap_t &banmap);
    void SetBanned(const banmap_t &banmap);

    void AddOneShot(const std::string& strDest);

    bool AddNode(const std::string& node);
    bool RemoveAddedNode(const std::string& node);
    std::vector<AddedNodeInfo> GetAddedNodeInfo();

    size_t GetNodeCount(NumConnections num);
    void GetNodeStats(std::vector<CNodeStats>& vstats);
    bool DisconnectAddress(const CNetAddr& addr);
    bool DisconnectNode(const std::string& node);
    bool DisconnectNode(NodeId id);
    bool DisconnectSubnet(const CSubNet& subnet);

    void AddWhitelistedRange(const CSubNet &subnet);
private:
    struct ListenSocket {
        SOCKET socket;
        bool whitelisted;

        ListenSocket(SOCKET socket_, bool whitelisted_) : socket(socket_), whitelisted(whitelisted_) {}
    };

    void ThreadOpenAddedConnections();
    void ProcessOneShot();
    void ThreadOpenConnections();
    void ThreadMessageHandler();
    void AcceptConnection(const ListenSocket& hListenSocket);
    void ThreadSocketHandler();
    void ThreadDNSAddressSeed();
    void ThreadMnbRequestConnections();

    CNode* FindNode(const CNetAddr& ip);
    CNode* FindNode(const CSubNet& subNet);
    CNode* FindNode(const std::string& addrName);
    CNode* FindNode(const CService& addr);

    bool AttemptToEvictConnection();
    bool IsWhitelistedRange(const CNetAddr &addr);

    void DeleteNode(CNode* pnode);

    NodeId GetNewNodeId();

    //!check is the banlist has unwritten changes
    bool BannedSetIsDirty();
    //!set the "dirty" flag for the banlist
    void SetBannedSetDirty(bool dirty=true);
    //!clean unused entries (if bantime has expired)
    void SweepBanned();
    void DumpAddresses();
    void DumpData();
    void DumpBanlist();

    // Whitelisted ranges. Any node connecting from these is automatically
    // whitelisted (as well as those connecting to whitelisted binds).
    std::vector<CSubNet> vWhitelistedRange;
    CCriticalSection cs_vWhitelistedRange;

    std::vector<ListenSocket> vhListenSocket;
    banmap_t setBanned;
    CCriticalSection cs_setBanned;
    bool setBannedIsDirty;
    bool fAddressesInitialized;
    CAddrMan addrman;
    std::deque<std::string> vOneShots;
    CCriticalSection cs_vOneShots;
    std::vector<std::string> vAddedNodes;
    CCriticalSection cs_vAddedNodes;
    std::vector<CNode*> vNodes;
    mutable CCriticalSection cs_vNodes;
    std::atomic<NodeId> nLastNodeId;
    boost::condition_variable messageHandlerCondition;
};
extern std::unique_ptr<CConnman> g_connman;
void MapPort(bool fUseUPnP);
unsigned short GetListenPort();
bool BindListenPort(const CService &bindAddr, std::string& strError, bool fWhitelisted = false);
bool StartNode(CConnman& connman, boost::thread_group& threadGroup, CScheduler& scheduler, std::string& strNodeError);
bool StopNode(CConnman& connman);
void SocketSendData(CNode *pnode);

struct CombinerAll
{
    typedef bool result_type;

    template<typename I>
    bool operator()(I first, I last) const
    {
        while (first != last) {
            if (!(*first)) return false;
            ++first;
        }
        return true;
    }
};

// Signals for message handling
struct CNodeSignals
{
    boost::signals2::signal<int ()> GetHeight;
    boost::signals2::signal<bool (CNode*, CConnman&), CombinerAll> ProcessMessages;
    boost::signals2::signal<bool (CNode*, CConnman&), CombinerAll> SendMessages;
    boost::signals2::signal<void (NodeId, const CNode*)> InitializeNode;
    boost::signals2::signal<void (NodeId, bool&)> FinalizeNode;
};


CNodeSignals& GetNodeSignals();


enum
{
    LOCAL_NONE,   // unknown
    LOCAL_IF,     // address a local interface listens on
    LOCAL_BIND,   // address explicit bound to
    LOCAL_UPNP,   // address reported by UPnP
    LOCAL_MANUAL, // address explicitly specified (-externalip=)

    LOCAL_MAX
};

bool IsPeerAddrLocalGood(CNode *pnode);
void AdvertiseLocal(CNode *pnode);
void SetLimited(enum Network net, bool fLimited = true);
bool IsLimited(enum Network net);
bool IsLimited(const CNetAddr& addr);
bool AddLocal(const CService& addr, int nScore = LOCAL_NONE);
bool AddLocal(const CNetAddr& addr, int nScore = LOCAL_NONE);
bool RemoveLocal(const CService& addr);
bool SeenLocal(const CService& addr);
bool IsLocal(const CService& addr);
bool GetLocal(CService &addr, const CNetAddr *paddrPeer = NULL);
bool IsReachable(enum Network net);
bool IsReachable(const CNetAddr &addr);
CAddress GetLocalAddress(const CNetAddr *paddrPeer = NULL);


extern bool fDiscover;
extern bool fListen;
extern ServiceFlags nLocalServices;

/** Maximum number of connections to simultaneously allow (aka connection slots) */
extern int nMaxConnections;

extern std::map<CInv, CDataStream> mapRelay;
extern std::deque<std::pair<int64_t, CInv> > vRelayExpiration;
extern CCriticalSection cs_mapRelay;
extern limitedmap<uint256, int64_t> mapAlreadyAskedFor;

/** Subversion as sent to the P2P network in `version` messages */
extern std::string strSubVersion;

struct LocalServiceInfo {
    int nScore;
    int nPort;
};

extern CCriticalSection cs_mapLocalHost;
extern std::map<CNetAddr, LocalServiceInfo> mapLocalHost;
typedef std::map<std::string, uint64_t> mapMsgCmdSize; //command, total bytes

class CNodeStats
{
public:
    NodeId nodeid;
    ServiceFlags nServices;
    bool fRelayTxes;
    int64_t nLastSend;
    int64_t nLastRecv;
    int64_t nTimeConnected;
    int64_t nTimeOffset;
    std::string addrName;
    int nVersion;
    std::string cleanSubVer;
    bool fInbound;
    int nStartingHeight;
    uint64_t nSendBytes;
    mapMsgCmdSize mapSendBytesPerMsgCmd;
    uint64_t nRecvBytes;
    mapMsgCmdSize mapRecvBytesPerMsgCmd;
    bool fWhitelisted;
    double dPingTime;
    double dPingWait;
    double dPingMin;
    std::string addrLocal;
};




class CNetMessage {
public:
    bool in_data;                   // parsing header (false) or data (true)

    CDataStream hdrbuf;             // partially received header
    CMessageHeader hdr;             // complete header
    unsigned int nHdrPos;

    CDataStream vRecv;              // received message data
    unsigned int nDataPos;

    int64_t nTime;                  // time (in microseconds) of message receipt.

    CNetMessage(const CMessageHeader::MessageStartChars& pchMessageStartIn, int nTypeIn, int nVersionIn) : hdrbuf(nTypeIn, nVersionIn), hdr(pchMessageStartIn), vRecv(nTypeIn, nVersionIn) {
        hdrbuf.resize(24);
        in_data = false;
        nHdrPos = 0;
        nDataPos = 0;
        nTime = 0;
    }

    bool complete() const
    {
        if (!in_data)
            return false;
        return (hdr.nMessageSize == nDataPos);
    }

    void SetVersion(int nVersionIn)
    {
        hdrbuf.SetVersion(nVersionIn);
        vRecv.SetVersion(nVersionIn);
    }

    int readHeader(const char *pch, unsigned int nBytes);
    int readData(const char *pch, unsigned int nBytes);
};


/** Information about a peer */
class CNode
{
public:
    // socket
    ServiceFlags nServices;
    ServiceFlags nServicesExpected;
    SOCKET hSocket;
    CDataStream ssSend;
    size_t nSendSize; // total size of all vSendMsg entries
    size_t nSendOffset; // offset inside the first vSendMsg already sent
    uint64_t nSendBytes;
    std::deque<CSerializeData> vSendMsg;
    CCriticalSection cs_vSend;

    std::deque<CInv> vRecvGetData;
    std::deque<CNetMessage> vRecvMsg;
    CCriticalSection cs_vRecvMsg;
    uint64_t nRecvBytes;
    int nRecvVersion;

    int64_t nLastSend;
    int64_t nLastRecv;
    int64_t nTimeConnected;
    int64_t nTimeOffset;
    int64_t nLastWarningTime;
    CAddress addr;
    std::string addrName;
    CService addrLocal;
    int nNumWarningsSkipped;
    int nVersion;
    // strSubVer is whatever byte array we read from the wire. However, this field is intended
    // to be printed out, displayed to humans in various forms and so on. So we sanitize it and
    // store the sanitized version in cleanSubVer. The original should be used when dealing with
    // the network or wire types and the cleaned string used when displayed or logged.
    std::string strSubVer, cleanSubVer;
    bool fWhitelisted; // This peer can bypass DoS banning.
    bool fFeeler; // If true this node is being used as a short lived feeler.
    bool fOneShot;
    bool fClient;
    bool fInbound;
    bool fNetworkNode;
    bool fSuccessfullyConnected;
    bool fDisconnect;
    // We use fRelayTxes for two purposes -
    // a) it allows us to not relay tx invs before receiving the peer's version message
    // b) the peer may tell us in its version message that we should not relay tx invs
    //    unless it loads a bloom filter.
    bool fRelayTxes;
    // If 'true' this node will be disconnected on CMasternodeMan::ProcessMasternodeConnections()
    bool fMasternode;
    CSemaphoreGrant grantOutbound;
    CSemaphoreGrant grantMasternodeOutbound;
    CCriticalSection cs_filter;
    CBloomFilter* pfilter;
    int nRefCount;
    NodeId id;
protected:

    mapMsgCmdSize mapSendBytesPerMsgCmd;
    mapMsgCmdSize mapRecvBytesPerMsgCmd;

    // Basic fuzz-testing
    void Fuzz(int nChance); // modifies ssSend

public:
    uint256 hashContinue;
    int nStartingHeight;

    // flood relay
    std::vector<CAddress> vAddrToSend;
    CRollingBloomFilter addrKnown;
    bool fGetAddr;
    std::set<uint256> setKnown;
    int64_t nNextAddrSend;
    int64_t nNextLocalAddrSend;

    // inventory based relay
    CRollingBloomFilter filterInventoryKnown;
    std::vector<CInv> vInventoryToSend;
    CCriticalSection cs_inventory;
    std::set<uint256> setAskFor;
    std::multimap<int64_t, CInv> mapAskFor;
    int64_t nNextInvSend;
    // Used for headers announcements - unfiltered blocks to relay
    // Also protected by cs_inventory
    std::vector<uint256> vBlockHashesToAnnounce;

    // Block and TXN accept times
    std::atomic<int64_t> nLastBlockTime;
    std::atomic<int64_t> nLastTXTime;

    // Ping time measurement:
    // The pong reply we're expecting, or 0 if no pong expected.
    uint64_t nPingNonceSent;
    // Time (in usec) the last ping was sent, or 0 if no ping was ever sent.
    int64_t nPingUsecStart;
    // Last measured round-trip time.
    int64_t nPingUsecTime;
    // Best measured round-trip time.
    int64_t nMinPingUsecTime;
    // Whether a ping is requested.
    bool fPingQueued;

    std::vector<unsigned char> vchKeyedNetGroup;

    CNode(NodeId id, SOCKET hSocketIn, const CAddress &addrIn, const std::string &addrNameIn = "", bool fInboundIn = false, bool fNetworkNodeIn = false);
    ~CNode();

private:
    // Network usage totals
    static CCriticalSection cs_totalBytesRecv;
    static CCriticalSection cs_totalBytesSent;
    static uint64_t nTotalBytesRecv;
    static uint64_t nTotalBytesSent;

    // outbound limit & stats
    static uint64_t nMaxOutboundTotalBytesSentInCycle;
    static uint64_t nMaxOutboundCycleStartTime;
    static uint64_t nMaxOutboundLimit;
    static uint64_t nMaxOutboundTimeframe;

    // Secret key for computing keyed net groups
    static std::vector<unsigned char> vchSecretKey;

    CCriticalSection cs_nRefCount;

    CNode(const CNode&);
    void operator=(const CNode&);

    uint64_t nLocalHostNonce;
public:

    NodeId GetId() const {
      return id;
    }

    uint64_t GetLocalNonce() const {
      return nLocalHostNonce;
    }

    int GetRefCount()
    {
        LOCK(cs_nRefCount);
        assert(nRefCount >= 0);
        return nRefCount;
    }

    // requires LOCK(cs_vRecvMsg)
    unsigned int GetTotalRecvSize()
    {
        unsigned int total = 0;
        BOOST_FOREACH(const CNetMessage &msg, vRecvMsg)
            total += msg.vRecv.size() + 24;
        return total;
    }

    // requires LOCK(cs_vRecvMsg)
    bool ReceiveMsgBytes(const char *pch, unsigned int nBytes, bool& complete);

    // requires LOCK(cs_vRecvMsg)
    void SetRecvVersion(int nVersionIn)
    {
        nRecvVersion = nVersionIn;
        BOOST_FOREACH(CNetMessage &msg, vRecvMsg)
            msg.SetVersion(nVersionIn);
    }

    CNode* AddRef()
    {
        LOCK(cs_nRefCount);
        nRefCount++;
        return this;
    }

    void Release()
    {
        LOCK(cs_nRefCount);
        nRefCount--;
        assert(nRefCount >= 0);
    }



    void AddAddressKnown(const CAddress& addr)
    {
        addrKnown.insert(addr.GetKey());
    }

    void PushAddress(const CAddress& addr)
    {
        // Known checking here is only to save space from duplicates.
        // SendMessages will filter it again for knowns that were added
        // after addresses were pushed.
        if (addr.IsValid() && !addrKnown.contains(addr.GetKey())) {
            if (vAddrToSend.size() >= MAX_ADDR_TO_SEND) {
                vAddrToSend[insecure_rand() % vAddrToSend.size()] = addr;
            } else {
                vAddrToSend.push_back(addr);
            }
        }
    }


    void AddInventoryKnown(const CInv& inv)
    {
        {
            LOCK(cs_inventory);
            filterInventoryKnown.insert(inv.hash);
        }
    }

    void PushInventory(const CInv& inv)
    {
        {
            LOCK(cs_inventory);
            if (inv.type == MSG_TX && filterInventoryKnown.contains(inv.hash)) {
                LogPrint("net", "PushInventory --  filtered inv: %s peer=%d\n", inv.ToString(), id);
                return;
            }
            LogPrint("net", "PushInventory --  inv: %s peer=%d\n", inv.ToString(), id);
            vInventoryToSend.push_back(inv);
        }
    }

    void PushBlockHash(const uint256 &hash)
    {
        LOCK(cs_inventory);
        vBlockHashesToAnnounce.push_back(hash);
    }

    void AskFor(const CInv& inv);

    // TODO: Document the postcondition of this function.  Is cs_vSend locked?
    void BeginMessage(const char* pszCommand) EXCLUSIVE_LOCK_FUNCTION(cs_vSend);

    // TODO: Document the precondition of this function.  Is cs_vSend locked?
    void AbortMessage() UNLOCK_FUNCTION(cs_vSend);

    // TODO: Document the precondition of this function.  Is cs_vSend locked?
    void EndMessage(const char* pszCommand) UNLOCK_FUNCTION(cs_vSend);

    void PushVersion();


    void PushMessage(const char* pszCommand)
    {
        try
        {
            BeginMessage(pszCommand);
            EndMessage(pszCommand);
        }
        catch (...)
        {
            AbortMessage();
            throw;
        }
    }

    template<typename T1>
    void PushMessage(const char* pszCommand, const T1& a1)
    {
        try
        {
            BeginMessage(pszCommand);
            ssSend << a1;
            EndMessage(pszCommand);
        }
        catch (...)
        {
            AbortMessage();
            throw;
        }
    }

    template<typename T1, typename T2>
    void PushMessage(const char* pszCommand, const T1& a1, const T2& a2)
    {
        try
        {
            BeginMessage(pszCommand);
            ssSend << a1 << a2;
            EndMessage(pszCommand);
        }
        catch (...)
        {
            AbortMessage();
            throw;
        }
    }

    template<typename T1, typename T2, typename T3>
    void PushMessage(const char* pszCommand, const T1& a1, const T2& a2, const T3& a3)
    {
        try
        {
            BeginMessage(pszCommand);
            ssSend << a1 << a2 << a3;
            EndMessage(pszCommand);
        }
        catch (...)
        {
            AbortMessage();
            throw;
        }
    }

    template<typename T1, typename T2, typename T3, typename T4>
    void PushMessage(const char* pszCommand, const T1& a1, const T2& a2, const T3& a3, const T4& a4)
    {
        try
        {
            BeginMessage(pszCommand);
            ssSend << a1 << a2 << a3 << a4;
            EndMessage(pszCommand);
        }
        catch (...)
        {
            AbortMessage();
            throw;
        }
    }

    template<typename T1, typename T2, typename T3, typename T4, typename T5>
    void PushMessage(const char* pszCommand, const T1& a1, const T2& a2, const T3& a3, const T4& a4, const T5& a5)
    {
        try
        {
            BeginMessage(pszCommand);
            ssSend << a1 << a2 << a3 << a4 << a5;
            EndMessage(pszCommand);
        }
        catch (...)
        {
            AbortMessage();
            throw;
        }
    }

    template<typename T1, typename T2, typename T3, typename T4, typename T5, typename T6>
    void PushMessage(const char* pszCommand, const T1& a1, const T2& a2, const T3& a3, const T4& a4, const T5& a5, const T6& a6)
    {
        try
        {
            BeginMessage(pszCommand);
            ssSend << a1 << a2 << a3 << a4 << a5 << a6;
            EndMessage(pszCommand);
        }
        catch (...)
        {
            AbortMessage();
            throw;
        }
    }

    template<typename T1, typename T2, typename T3, typename T4, typename T5, typename T6, typename T7>
    void PushMessage(const char* pszCommand, const T1& a1, const T2& a2, const T3& a3, const T4& a4, const T5& a5, const T6& a6, const T7& a7)
    {
        try
        {
            BeginMessage(pszCommand);
            ssSend << a1 << a2 << a3 << a4 << a5 << a6 << a7;
            EndMessage(pszCommand);
        }
        catch (...)
        {
            AbortMessage();
            throw;
        }
    }

    template<typename T1, typename T2, typename T3, typename T4, typename T5, typename T6, typename T7, typename T8>
    void PushMessage(const char* pszCommand, const T1& a1, const T2& a2, const T3& a3, const T4& a4, const T5& a5, const T6& a6, const T7& a7, const T8& a8)
    {
        try
        {
            BeginMessage(pszCommand);
            ssSend << a1 << a2 << a3 << a4 << a5 << a6 << a7 << a8;
            EndMessage(pszCommand);
        }
        catch (...)
        {
            AbortMessage();
            throw;
        }
    }

    template<typename T1, typename T2, typename T3, typename T4, typename T5, typename T6, typename T7, typename T8, typename T9>
    void PushMessage(const char* pszCommand, const T1& a1, const T2& a2, const T3& a3, const T4& a4, const T5& a5, const T6& a6, const T7& a7, const T8& a8, const T9& a9)
    {
        try
        {
            BeginMessage(pszCommand);
            ssSend << a1 << a2 << a3 << a4 << a5 << a6 << a7 << a8 << a9;
            EndMessage(pszCommand);
        }
        catch (...)
        {
            AbortMessage();
            throw;
        }
    }
 
    template<typename T1, typename T2, typename T3, typename T4, typename T5, typename T6, typename T7, typename T8, typename T9, typename T10>
    void PushMessage(const char* pszCommand, const T1& a1, const T2& a2, const T3& a3, const T4& a4, const T5& a5, const T6& a6, const T7& a7, const T8& a8, const T9& a9, const T10& a10)
    {
        try
        {
            BeginMessage(pszCommand);
            ssSend << a1 << a2 << a3 << a4 << a5 << a6 << a7 << a8 << a9 << a10;
            EndMessage(pszCommand);
        }
        catch (...)
        {
            AbortMessage();
            throw;
        }
    }

    template<typename T1, typename T2, typename T3, typename T4, typename T5, typename T6, typename T7, typename T8, typename T9, typename T10, typename T11>
    void PushMessage(const char* pszCommand, const T1& a1, const T2& a2, const T3& a3, const T4& a4, const T5& a5, const T6& a6, const T7& a7, const T8& a8, const T9& a9, const T10& a10, const T11& a11)
    {
        try
        {
            BeginMessage(pszCommand);
            ssSend << a1 << a2 << a3 << a4 << a5 << a6 << a7 << a8 << a9 << a10 << a11;
            EndMessage(pszCommand);
        }
        catch (...)
        {
            AbortMessage();
            throw;
        }
    }

    template<typename T1, typename T2, typename T3, typename T4, typename T5, typename T6, typename T7, typename T8, typename T9, typename T10, typename T11, typename T12>
    void PushMessage(const char* pszCommand, const T1& a1, const T2& a2, const T3& a3, const T4& a4, const T5& a5, const T6& a6, const T7& a7, const T8& a8, const T9& a9, const T10& a10, const T11& a11, const T12& a12)
    {
        try
        {
            BeginMessage(pszCommand);
            ssSend << a1 << a2 << a3 << a4 << a5 << a6 << a7 << a8 << a9 << a10 << a11 << a12;
            EndMessage(pszCommand);
        }
        catch (...)
        {
            AbortMessage();
            throw;
        }
    }

    void CloseSocketDisconnect();

    void copyStats(CNodeStats &stats);

    // Network stats
    static void RecordBytesRecv(uint64_t bytes);
    static void RecordBytesSent(uint64_t bytes);

    static uint64_t GetTotalBytesRecv();
    static uint64_t GetTotalBytesSent();

    //!set the max outbound target in bytes
    static void SetMaxOutboundTarget(uint64_t limit);
    static uint64_t GetMaxOutboundTarget();

    //!set the timeframe for the max outbound target
    static void SetMaxOutboundTimeframe(uint64_t timeframe);
    static uint64_t GetMaxOutboundTimeframe();

    //!check if the outbound target is reached
    // if param historicalBlockServingLimit is set true, the function will
    // response true if the limit for serving historical blocks has been reached
    static bool OutboundTargetReached(bool historicalBlockServingLimit);

    //!response the bytes left in the current max outbound cycle
    // in case of no limit, it will always response 0
    static uint64_t GetOutboundTargetBytesLeft();

    //!response the time in second left in the current max outbound cycle
    // in case of no limit, it will always response 0
    static uint64_t GetMaxOutboundTimeLeftInCycle();

    static std::vector<unsigned char> CalculateKeyedNetGroup(CAddress& address);
};

class CExplicitNetCleanup
{
public:
    static void callCleanup();
};



/** Return a timestamp in the future (in microseconds) for exponentially distributed events. */
int64_t PoissonNextSend(int64_t nNow, int average_interval_seconds);

#endif // BITCOIN_NET_H
