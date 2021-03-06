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

#include <openpeer/services/ITCPMessaging.h>
#include <openpeer/services/IBackgrounding.h>
#include <openpeer/services/internal/types.h>

#include <zsLib/Socket.h>
#include <zsLib/Timer.h>

#include <list>
#include <map>

#define OPENPEER_SERVICES_SETTING_TCPMESSAGING_BACKGROUNDING_PHASE "openpeer/services/backgrounding-phase-tcp-messaging"

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
      #pragma mark TCPMessaging
      #pragma mark

      class TCPMessaging : public Noop,
                           public zsLib::MessageQueueAssociator,
                           public ITCPMessaging,
                           public ITransportStreamReaderDelegate,
                           public ISocketDelegate,
                           public ITimerDelegate,
                           public IBackgroundingDelegate
      {
      public:
        friend interaction ITCPMessagingFactory;
        friend interaction ITCPMessaging;

      protected:
        TCPMessaging(
                     IMessageQueuePtr queue,
                     ITCPMessagingDelegatePtr delegate,
                     ITransportStreamPtr receiveStream,
                     ITransportStreamPtr sendStream,
                     bool framesHaveChannelNumber,
                     size_t maxMessageSizeInBytes = OPENPEER_SERVICES_ITCPMESSAGING_MAX_MESSAGE_SIZE_IN_BYTES
                     );

        TCPMessaging(Noop) :
          Noop(true),
          zsLib::MessageQueueAssociator(IMessageQueuePtr()) {}

        void init();

      public:
        ~TCPMessaging();

        static TCPMessagingPtr convert(ITCPMessagingPtr messaging);

      protected:
        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark TCPMessaging => ITCPMessaging
        #pragma mark

        static ElementPtr toDebug(ITCPMessagingPtr messaging);

        static TCPMessagingPtr accept(
                                      ITCPMessagingDelegatePtr delegate,
                                      ITransportStreamPtr receiveStream,
                                      ITransportStreamPtr sendStream,
                                      bool framesHaveChannelNumber,
                                      SocketPtr socket,
                                      size_t maxMessageSizeInBytes = OPENPEER_SERVICES_ITCPMESSAGING_MAX_MESSAGE_SIZE_IN_BYTES
                                      );

        static TCPMessagingPtr connect(
                                       ITCPMessagingDelegatePtr delegate,
                                       ITransportStreamPtr receiveStream,
                                       ITransportStreamPtr sendStream,
                                       bool framesHaveChannelNumber,
                                       IPAddress remoteIP,
                                       size_t maxMessageSizeInBytes = OPENPEER_SERVICES_ITCPMESSAGING_MAX_MESSAGE_SIZE_IN_BYTES
                                       );

        virtual PUID getID() const {return mID;}

        virtual ITCPMessagingSubscriptionPtr subscribe(ITCPMessagingDelegatePtr delegate);

        virtual void enableKeepAlive(bool enable = true);

        virtual void shutdown(Duration lingerTime = Seconds(OPENPEER_SERVICES_CLOSE_LINGER_TIMER_IN_SECONDS));

        virtual SessionStates getState(
                                       WORD *outLastErrorCode = NULL,
                                       String *outLastErrorReason = NULL
                                       ) const;

        virtual IPAddress getRemoteIP() const;

        virtual void setMaxMessageSizeInBytes(size_t maxMessageSizeInBytes);

        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark TCPMessaging => ITransportStreamReaderDelegate
        #pragma mark

        virtual void onTransportStreamReaderReady(ITransportStreamReaderPtr reader);

        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark TCPMessaging => ISocketDelegate
        #pragma mark

        virtual void onReadReady(SocketPtr socket);
        virtual void onWriteReady(SocketPtr socket);
        virtual void onException(SocketPtr socket);

        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark TCPMessaging => ITimerDelegate
        #pragma mark

        virtual void onTimer(TimerPtr timer);

        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark TCPMessaging => IBackgroundingDelegate
        #pragma mark

        virtual void onBackgroundingGoingToBackground(
                                                      IBackgroundingSubscriptionPtr subscription,
                                                      IBackgroundingNotifierPtr notifier
                                                      ) {}

        virtual void onBackgroundingGoingToBackgroundNow(IBackgroundingSubscriptionPtr subscription) {}

        virtual void onBackgroundingReturningFromBackground(IBackgroundingSubscriptionPtr subscription);

        virtual void onBackgroundingApplicationWillQuit(IBackgroundingSubscriptionPtr subscription) {}

      protected:
        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark TCPMessaging => (internal)
        #pragma mark

        bool isShuttingdown() const {return SessionState_ShuttingDown == mCurrentState;}
        bool isShutdown() const {return SessionState_Shutdown == mCurrentState;}

        RecursiveLock &getLock() const;
        Log::Params log(const char *message) const;
        Log::Params debug(const char *message) const;

        virtual ElementPtr toDebug() const;

        void setState(SessionStates state);
        void setError(WORD errorCode, const char *inReason = NULL);

        void cancel();
        void sendDataNow();
        bool sendQueuedData(size_t &outSent);

      protected:
        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark TCPMessaging => (data)
        #pragma mark

        AutoPUID mID;
        mutable RecursiveLock mLock;
        TCPMessagingWeakPtr mThisWeak;
        TCPMessagingPtr mGracefulShutdownReference;

        ITCPMessagingDelegateSubscriptions mSubscriptions;
        ITCPMessagingSubscriptionPtr mDefaultSubscription;

        IBackgroundingSubscriptionPtr mBackgroundingSubscription;

        SessionStates mCurrentState;

        AutoWORD mLastError;
        String mLastErrorReason;

        ITransportStreamWriterPtr mReceiveStream;
        ITransportStreamReaderPtr mSendStream;
        ITransportStreamReaderSubscriptionPtr mSendStreamSubscription;

        bool mFramesHaveChannelNumber;
        size_t mMaxMessageSizeInBytes;

        AutoBool mConnectIssued;
        AutoBool mTCPWriteReady;
        IPAddress mRemoteIP;
        SocketPtr mSocket;
        TimerPtr mLingerTimer;

        ByteQueuePtr mSendingQueue;
        ByteQueuePtr mReceivingQueue;
      };

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark ITCPMessagingFactory
      #pragma mark

      interaction ITCPMessagingFactory
      {
        static ITCPMessagingFactory &singleton();

        virtual TCPMessagingPtr accept(
                                       ITCPMessagingDelegatePtr delegate,
                                       ITransportStreamPtr receiveStream,
                                       ITransportStreamPtr sendStream,
                                       bool framesHaveChannelNumber,
                                       SocketPtr socket,
                                       size_t maxMessageSizeInBytes = OPENPEER_SERVICES_ITCPMESSAGING_MAX_MESSAGE_SIZE_IN_BYTES
                                       );

        virtual TCPMessagingPtr connect(
                                        ITCPMessagingDelegatePtr delegate,
                                        ITransportStreamPtr receiveStream,
                                        ITransportStreamPtr sendStream,
                                        bool framesHaveChannelNumber,
                                        IPAddress remoteIP,
                                        size_t maxMessageSizeInBytes = OPENPEER_SERVICES_ITCPMESSAGING_MAX_MESSAGE_SIZE_IN_BYTES
                                        );
      };
      
    }
  }
}
