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
#include <openpeer/services/internal/services_IRUDPChannelStream.h>
#include <openpeer/services/IRUDPChannel.h>
#include <openpeer/services/ISTUNRequester.h>
#include <openpeer/services/IWakeDelegate.h>

#include <zsLib/Timer.h>

#include <map>

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
      #pragma mark IRUDPChannelForRUDPICESocketSession
      #pragma mark

      interaction IRUDPChannelForRUDPICESocketSession
      {
        IRUDPChannelForRUDPICESocketSession &forSession() {return *this;}
        const IRUDPChannelForRUDPICESocketSession &forSession() const {return *this;}

        static RUDPChannelPtr createForRUDPICESocketSessionIncoming(
                                                                    IMessageQueuePtr queue,
                                                                    IRUDPChannelDelegateForSessionAndListenerPtr master,
                                                                    const IPAddress &remoteIP,
                                                                    WORD incomingChannelNumber,
                                                                    const char *localUsernameFrag,
                                                                    const char *localPassword,
                                                                    const char *remoteUsernameFrag,
                                                                    const char *remotePassword,
                                                                    STUNPacketPtr channelOpenPacket,
                                                                    STUNPacketPtr &outResponse
                                                                    );

        static RUDPChannelPtr createForRUDPICESocketSessionOutgoing(
                                                                    IMessageQueuePtr queue,
                                                                    IRUDPChannelDelegateForSessionAndListenerPtr master,
                                                                    IRUDPChannelDelegatePtr delegate,
                                                                    const IPAddress &remoteIP,
                                                                    WORD incomingChannelNumber,
                                                                    const char *localUsernameFrag,
                                                                    const char *localPassword,
                                                                    const char *remoteUsernameFrag,
                                                                    const char *remotePassword,
                                                                    const char *connectionInfo,
                                                                    ITransportStreamPtr receiveStream,
                                                                    ITransportStreamPtr sendStream
                                                                    );

        virtual PUID getID() const = 0;

        virtual void setDelegate(IRUDPChannelDelegatePtr delegate) = 0;
        virtual void setStreams(
                                ITransportStreamPtr receiveStream,
                                ITransportStreamPtr sendStream
                                ) = 0;

        virtual bool handleSTUN(
                                STUNPacketPtr stun,
                                STUNPacketPtr &outResponse,
                                const String &localUsernameFrag,
                                const String &remoteUsernameFrag
                                ) = 0;

        virtual void handleRUDP(
                                RUDPPacketPtr rudp,
                                const BYTE *buffer,
                                size_t bufferLengthInBytes
                                ) = 0;

        virtual void notifyWriteReady() = 0;
        virtual WORD getIncomingChannelNumber() const = 0;
        virtual WORD getOutgoingChannelNumber() const = 0;

        virtual void issueConnectIfNotIssued() = 0;

        virtual void shutdown() = 0;
        virtual void shutdownFromTimeout() = 0;
      };

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark IRUDPChannelForRUDPListener
      #pragma mark

      interaction IRUDPChannelForRUDPListener
      {
        IRUDPChannelForRUDPListener &forListener() {return *this;}
        const IRUDPChannelForRUDPListener &forListener() const {return *this;}

        static RUDPChannelPtr createForListener(
                                                IMessageQueuePtr queue,
                                                IRUDPChannelDelegateForSessionAndListenerPtr master,
                                                const IPAddress &remoteIP,
                                                WORD incomingChannelNumber,
                                                STUNPacketPtr channelOpenPacket,
                                                STUNPacketPtr &outResponse
                                                );

        virtual void setDelegate(IRUDPChannelDelegatePtr delegate) = 0;
        virtual void setStreams(
                                ITransportStreamPtr receiveStream,
                                ITransportStreamPtr sendStream
                                ) = 0;

        virtual bool handleSTUN(
                                STUNPacketPtr stun,
                                STUNPacketPtr &outResponse,
                                const String &localUsernameFrag,
                                const String &remoteUsernameFrag
                                ) = 0;

        virtual void handleRUDP(
                                RUDPPacketPtr rudp,
                                const BYTE *buffer,
                                size_t bufferLengthInBytes
                                ) = 0;

        virtual void notifyWriteReady() = 0;

        virtual void shutdown() = 0;
      };
      
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark RUDPChannel
      #pragma mark

      class RUDPChannel : public Noop,
                          public MessageQueueAssociator,
                          public IRUDPChannel,
                          public IRUDPChannelForRUDPICESocketSession,
                          public IRUDPChannelForRUDPListener,
                          public IWakeDelegate,
                          public IRUDPChannelStreamDelegate,
                          public ISTUNRequesterDelegate,
                          public ITimerDelegate
      {
      public:
        friend interaction IRUDPChannelFactory;
        friend interaction IRUDPChannel;

        typedef PUID ACKRequestID;
        typedef std::map<ACKRequestID, ISTUNRequesterPtr> ACKRequestMap;

        typedef std::pair<boost::shared_array<BYTE>, size_t> PendingSendBuffer;
        typedef std::list<PendingSendBuffer> PendingSendBufferList;

      protected:
        RUDPChannel(
                    IMessageQueuePtr queue,
                    IRUDPChannelDelegateForSessionAndListenerPtr master,
                    const IPAddress &remoteIP,
                    const char *localUserFrag,
                    const char *localPassword,
                    const char *remoteUserFrag,
                    const char *remotePassword,
                    DWORD minimumRTT,
                    DWORD lifetime,
                    WORD incomingChannelNumber,
                    QWORD localSequenceNumber,
                    const char *localChannelInfo,
                    WORD outgoingChannelNumber = 0,
                    QWORD remoteSequenceNumber = 0,
                    const char *remoteChannelInfo = NULL
                    );
        RUDPChannel(Noop) : Noop(true), MessageQueueAssociator(IMessageQueuePtr()) {};

        void init();

      public:
        ~RUDPChannel();

        static RUDPChannelPtr convert(IRUDPChannelPtr channel);

      protected:
        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark RUDPChannel => IRUDPChannel
        #pragma mark

        static String toDebugString(IRUDPChannelPtr channel, bool includeCommaPrefix = true);

        virtual PUID getID() const {return mID;}

        virtual RUDPChannelStates getState(
                                           WORD *outLastErrorCode = NULL,
                                           String *outLastErrorReason = NULL
                                           ) const;

        virtual void shutdown();

        virtual void shutdownDirection(Shutdown state);

        virtual IPAddress getConnectedRemoteIP();

        virtual String getRemoteConnectionInfo();

        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark RUDPChannel => IRUDPChannelForRUDPICESocketSession
        #pragma mark

        static RUDPChannelPtr createForRUDPICESocketSessionIncoming(
                                                                    IMessageQueuePtr queue,
                                                                    IRUDPChannelDelegateForSessionAndListenerPtr master,
                                                                    const IPAddress &remoteIP,
                                                                    WORD incomingChannelNumber,
                                                                    const char *localUserFrag,
                                                                    const char *localPassword,
                                                                    const char *remoteUserFrag,
                                                                    const char *remotePassword,
                                                                    STUNPacketPtr channelOpenPacket,
                                                                    STUNPacketPtr &outResponse
                                                                    );

        static RUDPChannelPtr createForRUDPICESocketSessionOutgoing(
                                                                    IMessageQueuePtr queue,
                                                                    IRUDPChannelDelegateForSessionAndListenerPtr master,
                                                                    IRUDPChannelDelegatePtr delegate,
                                                                    const IPAddress &remoteIP,
                                                                    WORD incomingChannelNumber,
                                                                    const char *localUserFrag,
                                                                    const char *localPassword,
                                                                    const char *remoteUserFrag,
                                                                    const char *remotePassword,
                                                                    const char *connectionInfo,
                                                                    ITransportStreamPtr receiveStream,
                                                                    ITransportStreamPtr sendStream
                                                                    );

        // (duplicate) virtual PUID getID() const;

        virtual void setDelegate(IRUDPChannelDelegatePtr delegate);
        virtual void setStreams(
                                ITransportStreamPtr receiveStream,
                                ITransportStreamPtr sendStream
                                );

        virtual bool handleSTUN(
                                STUNPacketPtr stun,
                                STUNPacketPtr &outResponse,
                                const String &localUsernameFrag,
                                const String &remoteUsernameFrag
                                );

        virtual void handleRUDP(
                                RUDPPacketPtr rudp,
                                const BYTE *buffer,
                                size_t bufferLengthInBytes
                                );

        virtual void notifyWriteReady();
        virtual WORD getIncomingChannelNumber() const;
        virtual WORD getOutgoingChannelNumber() const;

        virtual void issueConnectIfNotIssued();

        // (duplicate) virtual void shutdown();

        virtual void shutdownFromTimeout();

        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark RUDPChannel => IRUDPChannelForRUDPListener
        #pragma mark

        static RUDPChannelPtr createForListener(
                                                IMessageQueuePtr queue,
                                                IRUDPChannelDelegateForSessionAndListenerPtr master,
                                                const IPAddress &remoteIP,
                                                WORD incomingChannelNumber,
                                                STUNPacketPtr channelOpenPacket,
                                                STUNPacketPtr &outResponse
                                                );

        // (duplicate) virtual void setDelegate(IRUDPChannelDelegatePtr delegate);
        // virtual void setStreams(
        //                         ITransportStreamPtr receiveStream,
        //                         ITransportStreamPtr sendStream
        //                         );

        // (duplicate) virtual bool handleSTUN(
        //                                     STUNPacketPtr stun,
        //                                     STUNPacketPtr &outResponse,
        //                                     const String &localUsernameFrag,
        //                                     const String &remoteUsernameFrag
        //                                     );

        // (duplicate) virtual void handleRUDP(
        //                                     RUDPPacketPtr rudp,
        //                                     const BYTE *buffer,
        //                                     size_t bufferLengthInBytes
        //                                     );

        // (duplicate) virtual void notifyWriteReady();

        // (duplicate) virtual void shutdown();

        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark RUDPChannel => IWakeDelegate
        #pragma mark

        virtual void onWake();

        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark RUDPChannel => IRUDPChannelStreamDelegate
        #pragma mark

        virtual void onRUDPChannelStreamStateChanged(
                                                     IRUDPChannelStreamPtr stream,
                                                     RUDPChannelStreamStates state
                                                     );

        virtual bool notifyRUDPChannelStreamSendPacket(
                                                       IRUDPChannelStreamPtr stream,
                                                       const BYTE *packet,
                                                       size_t packetLengthInBytes
                                                       );

        virtual void onRUDPChannelStreamSendExternalACKNow(
                                                           IRUDPChannelStreamPtr stream,
                                                           bool guarenteeDelivery,
                                                           PUID guarenteeDeliveryRequestID = 0
                                                           );

        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark RUDPChannel => ISTUNRequesterDelegate
        #pragma mark

        virtual void onSTUNRequesterSendPacket(
                                               ISTUNRequesterPtr requester,
                                               IPAddress destination,
                                               boost::shared_array<BYTE> packet,
                                               size_t packetLengthInBytes
                                               );

        virtual bool handleSTUNRequesterResponse(
                                                 ISTUNRequesterPtr requester,
                                                 IPAddress fromIPAddress,
                                                 STUNPacketPtr response
                                                 );

        virtual void onSTUNRequesterTimedOut(ISTUNRequesterPtr requester);

        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark RUDPChannel => ITimerDelegate
        #pragma mark

        virtual void onTimer(TimerPtr timer);

      protected:
        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark RUDPChannel => (internal)
        #pragma mark

        String log(const char *message) const;
        void fix(STUNPacketPtr stun) const;

        bool isShuttingDown() {return RUDPChannelState_ShuttingDown == mCurrentState;}
        bool isShutdown() {return RUDPChannelState_Shutdown == mCurrentState;}

        virtual String getDebugValueString(bool includeCommaPrefix = true) const;

        void cancel(bool waitForAllDataToSend);
        void step();

        void setState(RUDPChannelStates state);
        void setError(WORD errorCode, const char *inReason = NULL);

        bool isValidIntegrity(STUNPacketPtr stun);
        void fillCredentials(STUNPacketPtr &outSTUN);
        void fillACK(STUNPacketPtr &outSTUN);

        bool handleStaleNonce(
                              ISTUNRequesterPtr &originalRequestVariable,
                              STUNPacketPtr response
                              );

      protected:
        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark RUDPChannel => (data)
        #pragma mark

        AutoPUID mID;
        mutable RecursiveLock mLock;
        RUDPChannelWeakPtr mThisWeak;
        RUDPChannelPtr mGracefulShutdownReference;

        AutoBool mIncoming;

        RUDPChannelStates mCurrentState;
        AutoWORD mLastError;
        String mLastErrorReason;

        IRUDPChannelDelegatePtr mDelegate;
        IRUDPChannelDelegateForSessionAndListenerPtr mMasterDelegate;

        ITransportStreamPtr mReceiveStream;
        ITransportStreamPtr mSendStream;

        IRUDPChannelStreamPtr mStream;
        ISTUNRequesterPtr mOpenRequest;
        ISTUNRequesterPtr mShutdownRequest;
        AutoBool mSTUNRequestPreviouslyTimedOut;    // if true then no need issue a "close" STUN request if a STUN request has previously timed out

        TimerPtr mTimer;

        IRUDPChannelStream::Shutdown mShutdownDirection;

        IPAddress mRemoteIP;

        String mLocalUsernameFrag;
        String mLocalPassword;
        String mRemoteUsernameFrag;
        String mRemotePassword;

        String mRealm;
        String mNonce;

        WORD mIncomingChannelNumber;
        WORD mOutgoingChannelNumber;

        QWORD mLocalSequenceNumber;
        QWORD mRemoteSequenceNumber;

        DWORD mMinimumRTT;
        DWORD mLifetime;

        String mLocalChannelInfo;
        String mRemoteChannelInfo;

        Time mLastSentData;
        Time mLastReceivedData;

        ACKRequestMap mOutstandingACKs;
      };

      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark IRUDPChannelDelegateForSessionAndListener
      #pragma mark

      interaction IRUDPChannelDelegateForSessionAndListener
      {
        typedef IRUDPChannel::RUDPChannelStates RUDPChannelStates;

        virtual void onRUDPChannelStateChanged(
                                               RUDPChannelPtr channel,
                                               RUDPChannelStates state
                                               ) = 0;

        //---------------------------------------------------------------------
        // PURPOSE: Send a packet over the socket interface to the remote party.
        virtual bool notifyRUDPChannelSendPacket(
                                                 RUDPChannelPtr channel,
                                                 const IPAddress &remoteIP,
                                                 const BYTE *packet,
                                                 size_t packetLengthInBytes
                                                 ) = 0;
      };

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark IRUDPChannelFactory
      #pragma mark

      interaction IRUDPChannelFactory
      {
        static IRUDPChannelFactory &singleton();

        virtual RUDPChannelPtr createForRUDPICESocketSessionIncoming(
                                                                     IMessageQueuePtr queue,
                                                                     IRUDPChannelDelegateForSessionAndListenerPtr master,
                                                                     const IPAddress &remoteIP,
                                                                     WORD incomingChannelNumber,
                                                                     const char *localUserFrag,
                                                                     const char *localPassword,
                                                                     const char *remoteUserFrag,
                                                                     const char *remotePassword,
                                                                     STUNPacketPtr channelOpenPacket,
                                                                     STUNPacketPtr &outResponse
                                                                     );

        virtual RUDPChannelPtr createForRUDPICESocketSessionOutgoing(
                                                                     IMessageQueuePtr queue,
                                                                     IRUDPChannelDelegateForSessionAndListenerPtr master,
                                                                     IRUDPChannelDelegatePtr delegate,
                                                                     const IPAddress &remoteIP,
                                                                     WORD incomingChannelNumber,
                                                                     const char *localUserFrag,
                                                                     const char *localPassword,
                                                                     const char *remoteUserFrag,
                                                                     const char *remotePassword,
                                                                     const char *connectionInfo,
                                                                     ITransportStreamPtr receiveStream,
                                                                     ITransportStreamPtr sendStream
                                                                     );

        virtual RUDPChannelPtr createForListener(
                                                 IMessageQueuePtr queue,
                                                 IRUDPChannelDelegateForSessionAndListenerPtr master,
                                                 const IPAddress &remoteIP,
                                                 WORD incomingChannelNumber,
                                                 STUNPacketPtr channelOpenPacket,
                                                 STUNPacketPtr &outResponse
                                                 );
      };
      
    }
  }
}


ZS_DECLARE_PROXY_BEGIN(openpeer::services::internal::IRUDPChannelDelegateForSessionAndListener)
ZS_DECLARE_PROXY_TYPEDEF(openpeer::services::internal::RUDPChannelPtr, RUDPChannelPtr)
ZS_DECLARE_PROXY_TYPEDEF(openpeer::services::internal::IRUDPChannelDelegateForSessionAndListener::RUDPChannelStates, RUDPChannelStates)
ZS_DECLARE_PROXY_METHOD_2(onRUDPChannelStateChanged, RUDPChannelPtr, RUDPChannelStates)
ZS_DECLARE_PROXY_METHOD_SYNC_RETURN_4(notifyRUDPChannelSendPacket, bool, RUDPChannelPtr, const IPAddress &, const BYTE *, size_t)
ZS_DECLARE_PROXY_END()
