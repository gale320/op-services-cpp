/*

 Copyright (c) 2013, SMB Phone Inc.
 All rights reserved.

 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions are met:

 1. Redistributions of source code must retain the above copyright notice, this
 list of conditions and the following disclaimer.
 2. Redistributions in binary form must reproduce the above copyright notice,
 this list of conditions and the following disclaimer in the documentation
 and/or other materials provided with the distribution.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

 The views and conclusions contained in the software and documentation are those
 of the authors and should not be interpreted as representing official policies,
 either expressed or implied, of the FreeBSD Project.

 */

#pragma once

#include <openpeer/services/internal/types.h>
#include <openpeer/services/IICESocket.h>
#include <openpeer/services/IDNS.h>
#include <openpeer/services/ITURNSocket.h>
#include <openpeer/services/ISTUNDiscovery.h>
#include <zsLib/types.h>
#include <zsLib/IPAddress.h>
#include <zsLib/MessageQueueAssociator.h>
#include <zsLib/Socket.h>
#include <zsLib/XML.h>
#include <zsLib/Timer.h>

#include <list>

namespace openpeer
{
  namespace services
  {
    namespace internal
    {
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark IICESocketForICESocketSession
      #pragma mark

      interaction IICESocketForICESocketSession
      {
        IICESocketForICESocketSession &forICESocketSession() {return *this;}
        const IICESocketForICESocketSession &forICESocketSession() const {return *this;}

        virtual IICESocketPtr getSocket() const = 0;

        virtual RecursiveLock &getLock() const = 0;

        virtual bool sendTo(
                            const IICESocket::Candidate &viaLocalCandidate,
                            const IPAddress &destination,
                            const BYTE *buffer,
                            size_t bufferLengthInBytes,
                            bool isUserData
                            ) = 0;

        virtual void addRoute(ICESocketSessionPtr session, const IPAddress &source) = 0;
        virtual void removeRoute(ICESocketSessionPtr session) = 0;

        virtual void onICESocketSessionClosed(PUID sessionID) = 0;
      };

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark ICESocket
      #pragma mark

      class ICESocket : public Noop,
                        public MessageQueueAssociator,
                        public IICESocket,
                        public ISocketDelegate,
                        public ITURNSocketDelegate,
                        public ISTUNDiscoveryDelegate,
                        public IICESocketForICESocketSession,
                        public ITimerDelegate
      {
      public:
        friend interaction IICESocketFactory;
        friend interaction IICESocket;

        typedef boost::shared_array<BYTE> RecycledPacketBuffer;
        typedef std::list<RecycledPacketBuffer> RecycledPacketBufferList;

        typedef std::list<IPAddress> IPAddressList;

        typedef std::map<PUID, ICESocketSessionPtr> ICESocketSessionMap;

        typedef std::map<IPAddress, ICESocketSessionPtr> QuickRouteMap;

        struct TURNInfo
        {
          TURNServerInfoPtr mServerInfo;

          ITURNSocketPtr    mTURNSocket;

          Time              mTURNRetryAfter;
          Duration          mTURNRetryDuration;
          TimerPtr          mTURNRetryTimer;

          CandidatePtr      mRelay;

          TURNInfo(
                   WORD componentID,
                   ULONG nextLocalPreference
                   );
        };

        struct STUNInfo
        {
          STUNServerInfoPtr mServerInfo;

          ISTUNDiscoveryPtr mSTUNDiscovery;

          CandidatePtr      mReflexive;

          STUNInfo(
                   WORD componentID,
                   ULONG nextLocalPreference
                   );
        };

        typedef boost::shared_ptr<TURNInfo> TURNInfoPtr;
        typedef boost::shared_ptr<STUNInfo> STUNInfoPtr;

        typedef std::map<TURNInfoPtr, TURNInfoPtr> TURNInfoMap;
        typedef std::map<ITURNSocketPtr, TURNInfoPtr> TURNInfoSocketMap;
        typedef std::map<IPAddress, TURNInfoPtr> TURNInfoRelatedIPMap;
        typedef std::map<STUNInfoPtr, STUNInfoPtr> STUNInfoMap;
        typedef std::map<ISTUNDiscoveryPtr, STUNInfoPtr> STUNInfoDiscoveryMap;

        struct LocalSocket
        {
          AutoPUID              mID;
          SocketPtr             mSocket;

          CandidatePtr          mLocal;

          TURNInfoMap           mTURNInfos;
          TURNInfoSocketMap     mTURNSockets;
          TURNInfoRelatedIPMap  mTURNRelayIPs;

          STUNInfoMap           mSTUNInfos;
          STUNInfoDiscoveryMap  mSTUNDiscoveries;

          LocalSocket(
                      WORD componentID,
                      ULONG nextLocalPreference
                      );

          void clearTURN(ITURNSocketPtr turnSocket);
          void clearSTUN(ISTUNDiscoveryPtr stunDiscovery);
        };

        typedef boost::shared_ptr<LocalSocket> LocalSocketPtr;
        typedef boost::weak_ptr<LocalSocket> LocalSocketWeakPtr;

        typedef IPAddress LocalIP;
        typedef std::map<LocalIP, LocalSocketPtr> LocalSocketIPAddressMap;
        typedef std::map<ITURNSocketPtr, LocalSocketPtr> LocalSocketTURNSocketMap;
        typedef std::map<ISTUNDiscoveryPtr, LocalSocketPtr> LocalSocketSTUNDiscoveryMap;
        typedef std::map<ISocketPtr, LocalSocketPtr> LocalSocketMap;

      protected:
        ICESocket(
                  IMessageQueuePtr queue,
                  IICESocketDelegatePtr delegate,
                  const TURNServerInfoList &turnServers,
                  const STUNServerInfoList &stunServers,
                  bool firstWORDInAnyPacketWillNotConflictWithTURNChannels,
                  WORD port,
                  IICESocketPtr foundationSocket
                  );
        ICESocket(Noop) : Noop(true), MessageQueueAssociator(IMessageQueuePtr()) {}

        void init();

      public:
        ~ICESocket();

        static ICESocketPtr convert(IICESocketPtr socket);

      protected:
        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark ICESocket => IICESocket
        #pragma mark

        static String toDebugString(IICESocketPtr socket, bool includeCommaPrefix = true);

        static ICESocketPtr create(
                                   IMessageQueuePtr queue,
                                   IICESocketDelegatePtr delegate,
                                   const TURNServerInfoList &turnServers,
                                   const STUNServerInfoList &stunServers,
                                   WORD port = 0,
                                   bool firstWORDInAnyPacketWillNotConflictWithTURNChannels = false,
                                   IICESocketPtr foundationSocket = IICESocketPtr()
                                   );

        virtual PUID getID() const {return mID;}

        virtual IICESocketSubscriptionPtr subscribe(IICESocketDelegatePtr delegate);

        virtual ICESocketStates getState(
                                         WORD *outLastErrorCode = NULL,
                                         String *outLastErrorReason = NULL
                                         ) const;

        virtual String getUsernameFrag() const;

        virtual String getPassword() const;

        virtual void shutdown();

        virtual void wakeup(Duration minimumTimeCandidatesMustRemainValidWhileNotUsed = Seconds(60*10));

        virtual void getLocalCandidates(
                                        CandidateList &outCandidates,
                                        String *outLocalCandidateVersion = NULL
                                        );
        virtual String getLocalCandidatesVersion() const;

        virtual IICESocketSessionPtr createSessionFromRemoteCandidates(
                                                                       IICESocketSessionDelegatePtr delegate,
                                                                       const char *remoteUsernameFrag,
                                                                       const char *remotePassword,
                                                                       const CandidateList &remoteCandidates,
                                                                       ICEControls control,
                                                                       IICESocketSessionPtr foundation = IICESocketSessionPtr()
                                                                       );

        virtual void monitorWriteReadyOnAllSessions(bool monitor = true);

        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark ICESocket => IICESocketForICESocketSession
        #pragma mark

        virtual IICESocketPtr getSocket() const {return mThisWeak.lock();}
        virtual RecursiveLock &getLock() const {return mLock;}

        virtual bool sendTo(
                            const Candidate &viaLocalCandidate,
                            const IPAddress &destination,
                            const BYTE *buffer,
                            size_t bufferLengthInBytes,
                            bool isUserData
                            );

        virtual void addRoute(ICESocketSessionPtr session, const IPAddress &source);
        virtual void removeRoute(ICESocketSessionPtr session);

        virtual void onICESocketSessionClosed(PUID sessionID);
        
        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark ICESocket => ISocketDelegate
        #pragma mark

        virtual void onReadReady(ISocketPtr socket);
        virtual void onWriteReady(ISocketPtr socket);
        virtual void onException(ISocketPtr socket);

        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark ICESocket => ITURNSocketDelegate
        #pragma mark

        virtual void onTURNSocketStateChanged(
                                              ITURNSocketPtr socket,
                                              TURNSocketStates state
                                              );

        virtual void handleTURNSocketReceivedPacket(
                                                    ITURNSocketPtr socket,
                                                    IPAddress source,
                                                    const BYTE *packet,
                                                    size_t packetLengthInBytes
                                                    );

        virtual bool notifyTURNSocketSendPacket(
                                                ITURNSocketPtr socket,
                                                IPAddress destination,
                                                const BYTE *packet,
                                                size_t packetLengthInBytes
                                                );

        virtual void onTURNSocketWriteReady(ITURNSocketPtr socket);

        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark ICESocket => ISTUNDiscoveryDelegate
        #pragma mark

        virtual void onSTUNDiscoverySendPacket(
                                               ISTUNDiscoveryPtr discovery,
                                               IPAddress destination,
                                               boost::shared_array<BYTE> packet,
                                               size_t packetLengthInBytes
                                               );

        virtual void onSTUNDiscoveryCompleted(ISTUNDiscoveryPtr discovery);

        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark ICESocket => ITimerDelegate
        #pragma mark

        virtual void onTimer(TimerPtr timer);

      protected:
        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark ICESocket => (internal)
        #pragma mark

        String log(const char *message) const;

        bool isShuttingDown() const {return ICESocketState_ShuttingDown == mCurrentState;}
        bool isShutdown() const {return ICESocketState_Shutdown == mCurrentState;}

        virtual String getDebugValueString(bool includeCommaPrefix = true) const;

        void cancel();
        
        void step();
        bool stepBind();
        bool stepSTUN();
        bool stepTURN();
        bool stepCandidates();

        void setState(ICESocketStates state);
        void setError(WORD errorCode, const char *inReason = NULL);

        bool getLocalIPs(IPAddressList &outIPs);
        void clearTURN(ITURNSocketPtr turn);
        void clearSTUN(ISTUNDiscoveryPtr stun);

        //---------------------------------------------------------------------
        // NOTE:  Do NOT call this method while in a lock because it must
        //        deliver data to delegates synchronously.
        void internalReceivedData(
                                  const Candidate &viaCandidate,
                                  const IPAddress &source,
                                  const BYTE *buffer,
                                  size_t bufferLengthInBytes
                                  );

        void getBuffer(RecycledPacketBuffer &outBuffer);
        void recycleBuffer(RecycledPacketBuffer &buffer);

      public:
        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark ICESocket::AutoRecycleBuffer
        #pragma mark

        class AutoRecycleBuffer
        {
        public:
          AutoRecycleBuffer(ICESocket &outer, RecycledPacketBuffer &buffer) : mOuter(outer), mBuffer(buffer) {}
          ~AutoRecycleBuffer() {mOuter.recycleBuffer(mBuffer);}
        private:
          ICESocket &mOuter;
          RecycledPacketBuffer &mBuffer;
        };

      protected:
        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark ICESocket (internal)
        #pragma mark

        AutoPUID              mID;
        mutable RecursiveLock mLock;
        ICESocketWeakPtr      mThisWeak;
        ICESocketPtr          mGracefulShutdownReference;

        IICESocketDelegateSubscriptions mSubscriptions;
        IICESocketSubscriptionPtr mDefaultSubscription;

        ICESocketStates     mCurrentState;
        AutoWORD            mLastError;
        String              mLastErrorReason;

        ICESocketPtr        mFoundation;
        AutoWORD            mComponentID;

        WORD                mBindPort;
        String              mUsernameFrag;
        String              mPassword;

        ULONG               mNextLocalPreference;

        LocalSocketIPAddressMap     mSocketLocalIPs;
        LocalSocketTURNSocketMap    mSocketTURNs;
        LocalSocketSTUNDiscoveryMap mSocketSTUNs;
        LocalSocketMap              mSockets;

        TimerPtr            mRebindTimer;
        Time                mRebindAttemptStartTime;
        AutoBool            mRebindCheckNow;

        bool                mMonitoringWriteReady;

        TURNServerInfoList  mTURNServers;
        STUNServerInfoList  mSTUNServers;
        bool                mFirstWORDInAnyPacketWillNotConflictWithTURNChannels;
        Time                mTURNLastUsed;                    // when was the TURN server last used to transport any data
        Duration            mTURNShutdownIfNotUsedBy;         // when will TURN be shutdown if it is not used by this time

        ICESocketSessionMap mSessions;

        QuickRouteMap       mRoutes;

        RecycledPacketBufferList mRecycledBuffers;

        AutoBool            mNotifiedCandidateChanged;
        DWORD               mLastCandidateCRC;
      };

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark IICESocketFactory
      #pragma mark

      interaction IICESocketFactory
      {
        static IICESocketFactory &singleton();

        virtual ICESocketPtr create(
                                    IMessageQueuePtr queue,
                                    IICESocketDelegatePtr delegate,
                                    const IICESocket::TURNServerInfoList &turnServers,
                                    const IICESocket::STUNServerInfoList &stunServers,
                                    WORD port = 0,
                                    bool firstWORDInAnyPacketWillNotConflictWithTURNChannels = false,
                                    IICESocketPtr foundationSocket = IICESocketPtr()
                                    );
      };
    }
  }
}

ZS_DECLARE_PROXY_BEGIN(openpeer::services::internal::IICESocketForICESocketSession)
ZS_DECLARE_PROXY_TYPEDEF(openpeer::services::IICESocketPtr, IICESocketPtr)
ZS_DECLARE_PROXY_TYPEDEF(openpeer::services::IICESocket, IICESocket)
ZS_DECLARE_PROXY_METHOD_SYNC_CONST_RETURN_0(getSocket, IICESocketPtr)
ZS_DECLARE_PROXY_METHOD_SYNC_CONST_RETURN_0(getLock, RecursiveLock &)
ZS_DECLARE_PROXY_METHOD_SYNC_RETURN_5(sendTo, bool, const IICESocket::Candidate &, const IPAddress &, const BYTE *, size_t, bool)
ZS_DECLARE_PROXY_METHOD_1(onICESocketSessionClosed, PUID)
ZS_DECLARE_PROXY_METHOD_SYNC_2(addRoute, openpeer::services::internal::ICESocketSessionPtr, const IPAddress &)
ZS_DECLARE_PROXY_METHOD_SYNC_1(removeRoute, openpeer::services::internal::ICESocketSessionPtr)
ZS_DECLARE_PROXY_END()
