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

#include <openpeer/services/internal/services_TURNSocket.h>
#include <openpeer/services/internal/services_Helper.h>
#include <openpeer/services/internal/services_wire.h>

#include <openpeer/services/ISettings.h>
#include <openpeer/services/STUNPacket.h>
#include <openpeer/services/ISTUNRequesterManager.h>

#include <zsLib/Socket.h>
#include <zsLib/Exception.h>
#include <zsLib/helpers.h>
#include <zsLib/Stringize.h>
#include <zsLib/XML.h>

#include <cryptopp/osrng.h>

#include <algorithm>

#define OPENPEER_SERVICES_TURNSOCKET_RECYCLE_BUFFER_SIZE (1 << (sizeof(WORD)*8))
#define OPENPEER_SERVICES_TURNSOCKET_MAX_RECYLCE_BUFFERS 4

#define OPENPEER_SERVICES_TURN_MINIMUM_KEEP_ALIVE_FOR_TURN_IN_SECONDS (20)             // keep alive should be 20 because ICE sends its keep alives every 15 seconds
#define OPENPEER_SERVICES_TURN_MINIMUM_LIFETIME_FOR_TURN_IN_SECONDS (15)               // do not allow server to dictate a LIFETIME lower than 15 seconds
#define OPENPEER_SERVICES_TURN_RECOMMENDED_REFRESH_BEFORE_LIFETIME_END_IN_SECONDS (60) // should try to refresh at least 60 seconds before LIFETIME of an allocation expires

#define OPENPEER_SERVICES_TURN_RECOMMENDED_LIFETIME_IN_SECONDS (60*10)                 // 10 minutes is recommended LIFETIME for an allocation on TURN
#define OPENPEER_SERVICES_TURN_PERMISSION_RETRY_IN_SECONDS (4*60)                      // 5 minutes until permissions expire so retry in 4 minutes
#define OPENPEER_SERVICES_TURN_REMOVE_PERMISSION_IF_NO_DATA_IN_SECONDS (10*60)         // remove any created permission if no data was sent in 10 minutes
#define OPENPEER_SERVICES_TURN_REMOVE_CHANNEL_IF_NO_DATA_IN_SECONDS (10*60)            // remove any channels created if no data was sent in 10 minutes

#define OPENPEER_SERVICES_TURN_ACTIVATE_NEXT_SERVER_IN_SECONDS (4)

namespace openpeer { namespace services { ZS_DECLARE_SUBSYSTEM(openpeer_services_turn) } }

namespace openpeer
{
  namespace services
  {
    namespace internal
    {
      using zsLib::ITimerDelegateProxy;
      typedef TURNSocket::IPAddressList IPAddressList;

      using zsLib::ISocketDelegateProxy;

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark (helpers)
      #pragma mark

      //-----------------------------------------------------------------------
      static size_t dwordBoundary(size_t length)
      {
        if (0 == (length % sizeof(DWORD)))
          return length;
        return length + (sizeof(DWORD) - (length % sizeof(DWORD)));
      }

      static const char *toString(ITURNSocket::TURNSocketStates state)
      {
        switch (state)
        {
          case ITURNSocket::TURNSocketState_Pending:      return "Pending";
          case ITURNSocket::TURNSocketState_Ready:        return "Ready";
          case ITURNSocket::TURNSocketState_ShuttingDown: return "Shutting down";
          case ITURNSocket::TURNSocketState_Shutdown:     return "Shutdown";
          default: break;
        }
        return "UNDEFINED";
      }

      static const char *toString(ITURNSocket::TURNSocketErrors error)
      {
        switch (error)
        {
          case ITURNSocket::TURNSocketError_None:                       return "None";
          case ITURNSocket::TURNSocketError_UserRequestedShutdown:      return "User requested shutdown";
          case ITURNSocket::TURNSocketError_DNSLookupFailure:           return "DNS lookup failure";
          case ITURNSocket::TURNSocketError_FailedToConnectToAnyServer: return "Failed to connect to any server";
          case ITURNSocket::TURNSocketError_RefreshTimeout:             return "Refresh timeout";
          case ITURNSocket::TURNSocketError_UnexpectedSocketFailure:    return "Unexpected socket failure";
          case ITURNSocket::TURNSocketError_BogusDataOnSocketReceived:  return "Bogus data on socket received";
          default: break;
        }
        return "UNDEFINED";
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark TURNSocket
      #pragma mark

      //-----------------------------------------------------------------------
      TURNSocket::TURNSocket(
                             IMessageQueuePtr queue,
                             ITURNSocketDelegatePtr delegate,
                             const char *turnServer,
                             const char *turnServerUsername,
                             const char *turnServerPassword,
                             bool useChannelBinding,
                             WORD limitChannelToRangeStart,
                             WORD limitChannelRoRangeEnd
                             ) :
        MessageQueueAssociator(queue),
        mID(zsLib::createPUID()),
        mCurrentState(TURNSocketState_Pending),
        mLastError(TURNSocketError_None),
        mDelegate(ITURNSocketDelegateProxy::createWeak(queue, delegate)),
        mServerName(turnServer ? turnServer : ""),
        mUsername(turnServerUsername ? turnServerUsername : ""),
        mPassword(turnServerPassword ? turnServerPassword : ""),
        mUseChannelBinding(useChannelBinding),
        mLimitChannelToRangeStart(limitChannelToRangeStart),
        mLimitChannelToRangeEnd(limitChannelRoRangeEnd),
        mLifetime(0),
        mLastSentDataToServer(zsLib::now()),
        mLastRefreshTimerWasSentAt(zsLib::now()),
        mPermissionRequesterMaxCapacity(0),
        mForceTURNUseUDP(ISettings::getBool(OPENPEER_SERVICES_SETTING_FORCE_TURN_TO_USE_UDP)),
        mForceTURNUseTCP(ISettings::getBool(OPENPEER_SERVICES_SETTING_FORCE_TURN_TO_USE_UDP))
      {
        ZS_THROW_INVALID_USAGE_IF(mLimitChannelToRangeStart > mLimitChannelToRangeEnd)
        ZS_LOG_DETAIL(log("created"))
      }

      //-----------------------------------------------------------------------
      TURNSocket::TURNSocket(
                             IMessageQueuePtr queue,
                             ITURNSocketDelegatePtr delegate,
                             IDNS::SRVResultPtr srvTURNUDP,
                             IDNS::SRVResultPtr srvTURNTCP,
                             const char *turnServerUsername,
                             const char *turnServerPassword,
                             bool useChannelBinding,
                             WORD limitChannelToRangeStart,
                             WORD limitChannelRoRangeEnd
                             ) :
        MessageQueueAssociator(queue),
        mID(zsLib::createPUID()),
        mCurrentState(TURNSocketState_Pending),
        mLastError(TURNSocketError_None),
        mDelegate(ITURNSocketDelegateProxy::createWeak(queue, delegate)),
        mTURNUDPSRVResult(IDNS::cloneSRV(srvTURNUDP)),
        mTURNTCPSRVResult(IDNS::cloneSRV(srvTURNTCP)),
        mUsername(turnServerUsername),
        mPassword(turnServerPassword),
        mUseChannelBinding(useChannelBinding),
        mLimitChannelToRangeStart(limitChannelToRangeStart),
        mLimitChannelToRangeEnd(limitChannelRoRangeEnd),
        mLifetime(0),
        mLastSentDataToServer(zsLib::now()),
        mLastRefreshTimerWasSentAt(zsLib::now()),
        mPermissionRequesterMaxCapacity(0),
        mForceTURNUseUDP(ISettings::getBool(OPENPEER_SERVICES_SETTING_FORCE_TURN_TO_USE_UDP)),
        mForceTURNUseTCP(ISettings::getBool(OPENPEER_SERVICES_SETTING_FORCE_TURN_TO_USE_UDP))
      {
        ZS_THROW_INVALID_USAGE_IF(mLimitChannelToRangeStart > mLimitChannelToRangeEnd)
        ZS_LOG_BASIC(log("created"))
      }

      //-----------------------------------------------------------------------
      void TURNSocket::init()
      {
        IHelper::setSocketThreadPriority();
        IHelper::setTimerThreadPriority();

        AutoRecursiveLock lock(mLock);
        ZS_LOG_DETAIL(debug("init"))

        String restricted = ISettings::getString(OPENPEER_SERVICES_SETTING_ONLY_ALLOW_TURN_TO_RELAY_DATA_TO_SPECIFIC_IPS);
        Helper::parseIPs(restricted, mRestrictedIPs);

        mBackgroundingSubscription = IBackgrounding::subscribe(
                                                               mThisWeak.lock(),
                                                               ISettings::getUInt(OPENPEER_SERVICES_SETTING_TURN_BACKGROUNDING_PHASE)
                                                               );

        step();
      }

      //-----------------------------------------------------------------------
      TURNSocket::~TURNSocket()
      {
        if(isNoop()) return;
        
        mThisWeak.reset();
        ZS_LOG_DETAIL(log("destroyed"))
        cancel();
      }

      //-----------------------------------------------------------------------
      TURNSocketPtr TURNSocket::convert(ITURNSocketPtr socket)
      {
        return dynamic_pointer_cast<TURNSocket>(socket);
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark TURNSocket => ITURNSocket
      #pragma mark

      //-----------------------------------------------------------------------
      TURNSocketPtr TURNSocket::create(
                                       IMessageQueuePtr queue,
                                       ITURNSocketDelegatePtr delegate,
                                       const char *turnServer,
                                       const char *turnServerUsername,
                                       const char *turnServerPassword,
                                       bool useChannelBinding,
                                       WORD limitChannelToRangeStart,
                                       WORD limitChannelRoRangeEnd
                                       )
      {
        ZS_THROW_INVALID_USAGE_IF(!queue)
        ZS_THROW_INVALID_USAGE_IF(!delegate)
        ZS_THROW_INVALID_USAGE_IF(!turnServer)

        TURNSocketPtr pThis(new TURNSocket(queue, delegate, turnServer, turnServerUsername, turnServerPassword, useChannelBinding, limitChannelToRangeStart, limitChannelRoRangeEnd));
        pThis->mThisWeak = pThis;
        pThis->init();
        return pThis;
      }

      //-----------------------------------------------------------------------
      TURNSocketPtr TURNSocket::create(
                                       IMessageQueuePtr queue,
                                       ITURNSocketDelegatePtr delegate,
                                       IDNS::SRVResultPtr srvTURNUDP,
                                       IDNS::SRVResultPtr srvTURNTCP,
                                       const char *turnServerUsername,
                                       const char *turnServerPassword,
                                       bool useChannelBinding,
                                       WORD limitChannelToRangeStart,
                                       WORD limitChannelRoRangeEnd
                                       )
      {
        ZS_THROW_INVALID_USAGE_IF(!queue)
        ZS_THROW_INVALID_USAGE_IF(!delegate)
        ZS_THROW_INVALID_USAGE_IF((!srvTURNUDP) && (!srvTURNTCP))

        TURNSocketPtr pThis(new TURNSocket(queue, delegate, srvTURNUDP, srvTURNTCP, turnServerUsername, turnServerPassword, useChannelBinding, limitChannelToRangeStart, limitChannelRoRangeEnd));
        pThis->mThisWeak = pThis;
        pThis->init();
        return pThis;
      }

      //-----------------------------------------------------------------------
      ElementPtr TURNSocket::toDebug(ITURNSocketPtr socket)
      {
        if (!socket) return ElementPtr();
        return TURNSocket::convert(socket)->toDebug();
      }

      //-----------------------------------------------------------------------
      ITURNSocket::TURNSocketStates TURNSocket::getState() const
      {
        AutoRecursiveLock lock(mLock);
        return mCurrentState;
      }

      //-----------------------------------------------------------------------
      ITURNSocket::TURNSocketErrors TURNSocket::getLastError() const
      {
        AutoRecursiveLock lock(mLock);
        return mLastError;
      }

      //-----------------------------------------------------------------------
      bool TURNSocket::isRelayingUDP() const
      {
        AutoRecursiveLock lock(mLock);
        if (!isReady()) {
          ZS_LOG_WARNING(Detail, log("not ready so can't be relaying UDP"))
          return false;
        }

        ZS_THROW_INVALID_ASSUMPTION_IF(!mActiveServer)

        ZS_LOG_DEBUG(log("is relaying UDP") + ZS_PARAM("relaying UDP", mActiveServer->mIsUDP))
        return mActiveServer->mIsUDP;
      }

      //-----------------------------------------------------------------------
      void TURNSocket::shutdown()
      {
        AutoRecursiveLock lock(mLock);
        mLastError = TURNSocketError_UserRequestedShutdown;
        cancel(); // do a graceful shutdown
      }

      //-----------------------------------------------------------------------
      bool TURNSocket::sendPacket(
                                  IPAddress destination,
                                  const BYTE *buffer,
                                  size_t bufferLengthInBytes,
                                  bool bindChannelIfPossible
                                  )
      {
        OPENPEER_SERVICES_WIRE_LOG_TRACE(log("send packet") + ZS_PARAM("destination", destination.string()) + ZS_PARAM("buffer length", bufferLengthInBytes) + ZS_PARAM("bind channel", bindChannelIfPossible))

        if (destination.isAddressEmpty()) {
          OPENPEER_SERVICES_WIRE_LOG_WARNING(Debug, log("cannot send packet over TURN as destination is invalid"))
          return false;
        }
        if (destination.isPortEmpty()) {
          OPENPEER_SERVICES_WIRE_LOG_WARNING(Debug, log("cannot send packet over TURN as destination port is invalid") + ZS_PARAM("ip", destination.string()))
          return false;
        }

        if (!Helper::containsIP(mRestrictedIPs, destination)) {
          ZS_LOG_WARNING(Trace, log("preventing packet from going via TURN server to destination as destination is not in restricted IP list") + ZS_PARAM("destination", destination.string()))
          return true;
        }

        if (NULL == buffer) {
          OPENPEER_SERVICES_WIRE_LOG_WARNING(Debug, log("cannot send packet as buffer is NULL"))
          return false;
        }
        if (0 == bufferLengthInBytes) {
          OPENPEER_SERVICES_WIRE_LOG_WARNING(Debug, log("cannot send packet as buffer length is empty"))
          return false;
        }
        if (bufferLengthInBytes > OPENPEER_SERVICES_TURN_MAX_CHANNEL_DATA_IN_BYTES) {
          OPENPEER_SERVICES_WIRE_LOG_WARNING(Debug, log("cannot send packet as buffer length is greater than maximum capacity") + ZS_PARAM("size", bufferLengthInBytes))
          return false;  // illegal to be so large
        }

        SecureByteBlockPtr packet;
        ServerPtr server;

        do
        {
          AutoRecursiveLock lock(mLock);
          if (mActiveServer) {
            mActiveServer->mInformedWriteReady = false;  // if this method was called in response to a write ready be sure to clear off the write-ready event so it can fire again
          }

          if (!isReady()) {
            OPENPEER_SERVICES_WIRE_LOG_WARNING(Detail, log("cannot send packet as TURN is not ready"))
            return false;
          }

          ZS_THROW_INVALID_ASSUMPTION_IF(!mActiveServer)
          server = mActiveServer;

          // scope: first, we check if there is a binded channel to this address
          {
            ChannelIPMap::iterator found = mChannelIPMap.find(destination);
            if (found != mChannelIPMap.end()) {
              // we found, but is it activated yet?
              ChannelInfoPtr info = (*found).second;
              if (info->mBound) {
                // yes, it is active, so we can packetize this in a special way to send to the remote peer
                packet = SecureByteBlockPtr(new SecureByteBlock(sizeof(DWORD)+dwordBoundary(bufferLengthInBytes)));

                ((WORD *)(packet->BytePtr()))[0] = htons(info->mChannelNumber);
                ((WORD *)(packet->BytePtr()))[1] = htons((WORD)bufferLengthInBytes);

                info->mLastSentDataAt = zsLib::now();

                // copy the entire buffer into the packet
                memcpy(&((packet->BytePtr())[sizeof(DWORD)]), buffer, bufferLengthInBytes);
                OPENPEER_SERVICES_WIRE_LOG_TRACE(log("sending packet via bound channel") + ZS_PARAM("channel", info->mChannelNumber) + ZS_PARAM("destination", destination.string()) + ZS_PARAM("buffer length", bufferLengthInBytes) + ZS_PARAM("bind channel", bindChannelIfPossible))
                break;
              }

              // the binding isn't active yet... will have to wrap the packet in a larger header until the binding request completes

            } else if (bindChannelIfPossible) {

              // there was no channel for this destination - try to bind a new one now...
              WORD freeChannelNumber = getNextChannelNumber();
              if ((0 != freeChannelNumber) &&
                  (mUseChannelBinding)) {

                ZS_LOG_DEBUG(log("will attempt to bind channel") + ZS_PARAM("channel", freeChannelNumber) + ZS_PARAM("ip", destination.string()))

                // we have a free channel... create a new binding... (won't happen immediately but hopefully the request will succeed)
                ChannelInfoPtr info = ChannelInfo::create();
                info->mChannelNumber = freeChannelNumber;
                info->mPeerAddress = destination;
                mChannelIPMap[destination] = info;
                mChannelNumberMap[freeChannelNumber] = info;

                IWakeDelegateProxy::create(mThisWeak.lock())->onWake();
              }
            }
          }

          STUNPacketPtr sendData = STUNPacket::createIndication(STUNPacket::Method_Send, NULL); // do not announce software in SEND to maximize data possible
          fix(sendData);
          sendData->mPeerAddressList.push_back(destination);
          sendData->mData = buffer;
          sendData->mDataLength = bufferLengthInBytes;

          packet = sendData->packetize(STUNPacket::RFC_5766_TURN);

          // scope: we need to check if there is a permission set to be able to even contact this address
          {
            PermissionMap::iterator found = mPermissions.find(destination);
            if (found == mPermissions.end()) {
              ZS_LOG_DEBUG(log("will attempt to create permision") + ZS_PARAM("ip", destination.string()))

              // we do not have a permission yet to send to this address so we need to create one...
              PermissionPtr permission = Permission::create();

              permission->mPeerAddress = destination;
              permission->mPendingData.push_back(packet);

              mPermissions[destination] = permission;

              // since the permission isn't installed yet we can't send the data just yet... best kick start that permission now...
              (IWakeDelegateProxy::create(mThisWeak.lock()))->onWake();
              return true;
            }

            PermissionPtr permission = (*found).second;
            permission->mLastSentDataAt = zsLib::now();

            if (!permission->mInstalled) {
              // the permission hasn't been installed yet so we still can't send the data...
              permission->mPendingData.push_back(packet);
              return true;
            }
          }
        } while(false);

        ZS_THROW_BAD_STATE_IF(!packet)  // how is this possible?

        // we are free to send the data now since there is a permission installed...
        return sendPacketOrDopPacketIfBufferFull(server, *packet, packet->SizeInBytes());
      }

      //-----------------------------------------------------------------------
      IPAddress TURNSocket::getActiveServerIP()
      {
        AutoRecursiveLock lock(mLock);
        if (!mActiveServer) return IPAddress();
        return mActiveServer->mServerIP;
      }

      //-----------------------------------------------------------------------
      IPAddress TURNSocket::getRelayedIP()
      {
        AutoRecursiveLock lock(mLock);
        return mRelayedIP;
      }

      //-----------------------------------------------------------------------
      IPAddress TURNSocket::getReflectedIP()
      {
        AutoRecursiveLock lock(mLock);
        return mReflectedIP;
      }

      //-----------------------------------------------------------------------
      bool TURNSocket::handleSTUNPacket(
                                        IPAddress fromIPAddress,
                                        STUNPacketPtr turnPacket
                                        )
      {
        ZS_THROW_INVALID_USAGE_IF(!turnPacket)

        if (STUNPacket::Method_Data != turnPacket->mMethod) {
          // see if the ISTUNRequesterManager can handle this packet...
          return (bool)ISTUNRequesterManager::handleSTUNPacket(fromIPAddress, turnPacket);
        }

        if (!turnPacket->isLegal(STUNPacket::RFC_5766_TURN)) return false;    // ignore if this isn't legal for TURN

        ITURNSocketDelegatePtr delegate;

        {
          AutoRecursiveLock lock(mLock);
          if (isShutdown()) return false;
          if (!mDelegate) return false;

          delegate = mDelegate;
        }

        // this is definately a TURN packet - handle the data within the packet...

        try {
          // send the packet to the delegate which is interested in the data received
          delegate->handleTURNSocketReceivedPacket(mThisWeak.lock(), turnPacket->mPeerAddressList.front(), turnPacket->mData, turnPacket->mDataLength);
        } catch (ITURNSocketDelegateProxy::Exceptions::DelegateGone &) {
          cancel();
          return true;
        }
        return true;
      }

      //-----------------------------------------------------------------------
      bool TURNSocket::handleChannelData(
                                         IPAddress fromIPAddress,
                                         const BYTE *buffer,
                                         size_t bufferLengthInBytes
                                         )
      {
        // WARNING: Do not call any delegates synchronously from within this
        //          method as this method was called from a synchronous
        //          delegate already.
        ZS_THROW_INVALID_USAGE_IF(!buffer)
        ZS_THROW_INVALID_USAGE_IF(0 == bufferLengthInBytes)

        {
          AutoRecursiveLock lock(mLock);
          if (isShutdown()) return false;
          if (!mDelegate) return false;
          if (!mUseChannelBinding) return false;                  // can't be bound channels if TURN isn't allowed to use it
          if (fromIPAddress != mAllocateResponseIP) return false; // must come from the TURN server
        }

        if (bufferLengthInBytes < sizeof(DWORD)) return false;

        WORD channel = ntohs(((WORD *)buffer)[0]);
        WORD length = ntohs(((WORD *)buffer)[1]);

        if ((channel < mLimitChannelToRangeStart) ||
            (channel > mLimitChannelToRangeEnd)) return false;        // this can't be legal channel data

        if (length > OPENPEER_SERVICES_TURN_MAX_CHANNEL_DATA_IN_BYTES) return false;  // this can't be legal channel data
        if (length > (bufferLengthInBytes-sizeof(DWORD))) {
          OPENPEER_SERVICES_WIRE_LOG_WARNING(Detail, log("channel packet received with a length set too large") + ZS_PARAM("ip", fromIPAddress.string()) + ZS_PARAM("reported length", length) + ZS_PARAM("actual length", bufferLengthInBytes))
          return false;
        }

        const BYTE *realBuffer = (const BYTE *)(&(((WORD *)buffer)[2]));

        ITURNSocketDelegatePtr delegate;
        IPAddress peerAddress;

        // scope: found out which channel this date belongs
        {
          AutoRecursiveLock lock(mLock);

          delegate = mDelegate;

          ChannelNumberMap::iterator found = mChannelNumberMap.find(channel);
          if (mChannelNumberMap.end() == found) {
            OPENPEER_SERVICES_WIRE_LOG_WARNING(Detail, log("channel packet received for non-existant channel") + ZS_PARAM("ip", fromIPAddress.string()) + ZS_PARAM("channel", channel))
            return false;                             // this isn't any bound channel we know about...
          }

          ChannelInfoPtr info = (*found).second;
          peerAddress = info->mPeerAddress;
        }

        try {
          // send the packet to the delegate which is interested in the data received
          delegate->handleTURNSocketReceivedPacket(mThisWeak.lock(), peerAddress, realBuffer, length);
        } catch (ITURNSocketDelegateProxy::Exceptions::DelegateGone &) {
          cancel();
          return true;
        }

        // the packet was handled as channel data
        return true;
      }

      //-----------------------------------------------------------------------
      void TURNSocket::notifyWriteReady()
      {
        AutoRecursiveLock lock(mLock);

        if (mActiveServer) {
          if (!mActiveServer->mIsUDP) {
            OPENPEER_SERVICES_WIRE_LOG_TRACE(log("notified delegate sender is write ready however we are sending via TCP so we will ignore this notification"))
            return;
          }
        }

        informWriteReady();
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark TURNSocket => IWakeDelegate
      #pragma mark

      //-----------------------------------------------------------------------
      void TURNSocket::onWake()
      {
        AutoRecursiveLock lock(mLock);
        step();
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark TURNSocket => ISTUNRequesterDelegate
      #pragma mark

      //-----------------------------------------------------------------------
      void TURNSocket::onSTUNRequesterSendPacket(
                                                 ISTUNRequesterPtr requester,
                                                 IPAddress destination,
                                                 SecureByteBlockPtr packet
                                                 )
      {
        ServerPtr server;

        // scope: we can't be in the context of a lock during that send
        {
          AutoRecursiveLock lock(mLock);
          if (isShutdown()) {
            ZS_LOG_WARNING(Detail, log("STUN requester wanting to send packet after shutdown"))
            return;
          }

          STUNPacketPtr request = requester->getRequest();
          if (STUNPacket::Method_Allocate == request->mMethod) {
            for (ServerList::iterator iter = mServers.begin(); iter != mServers.end(); ++iter)
            {
              ServerPtr &serverCompare = (*iter);
              if (serverCompare->mAllocateRequester == requester) {
                server = serverCompare;
                break;
              }
            }
          } else {
            server = mActiveServer;
          }

          if (!server) {
            ZS_LOG_WARNING(Detail, log("could not find a server to send the request to"))
            return;
          }
        }

        sendPacketOrDopPacketIfBufferFull(server, *packet, packet->SizeInBytes());
      }

      //-----------------------------------------------------------------------
      bool TURNSocket::handleSTUNRequesterResponse(
                                                   ISTUNRequesterPtr requester,
                                                   IPAddress fromIPAddress,
                                                   STUNPacketPtr response
                                                   )
      {
        // scope: we can't be in the middle of a lock while we call the handlePermissionRequester method
        {
          AutoRecursiveLock lock(mLock);
          // we now have a response to one of the requesters

          if (handleAllocateRequester(requester, fromIPAddress, response)) return true;

          if (handleRefreshRequester(requester, response)) return true;

          if (handleDeallocRequester(requester, response)) return true;

          // this could be a respond to a channel bind request... check so now...
          if (handleChannelRequester(requester, response)) return true;
        }
        // this could be a respond to a permission request... check so now...
        if (handlePermissionRequester(requester, response)) return true;

        return false;
      }

      //-----------------------------------------------------------------------
      void TURNSocket::onSTUNRequesterTimedOut(ISTUNRequesterPtr requester)
      {
        AutoRecursiveLock lock(mLock);
        if (isShutdown()) {
          ZS_LOG_WARNING(Detail, log("notified of STUN request timeout after shutdown"))
          return;
        }

        for (ServerList::iterator allocIter = mServers.begin(); allocIter != mServers.end(); )
        {
          ServerList::iterator current = allocIter;
          ++allocIter;

          ServerPtr &server = (*current);
          if (requester == server->mAllocateRequester) {
            ZS_LOG_WARNING(Detail, log("allocate request timed out") + ZS_PARAM("server IP", server->mServerIP.string()))
            mServers.erase(current);

            step();
            return;
          }
        }

        if (requester == mRefreshRequester) {
          ZS_LOG_WARNING(Detail, log("refresh requester timed out thus issuing shutdown"))
          clearRefreshRequester();
          // this is bad... but what can we do?? this is considered shutdown now...
          mLastError = TURNSocketError_RefreshTimeout;
          cancel();
          return;
        }

        if (requester == mDeallocateRequester) {
          ZS_LOG_WARNING(Detail, log("dealloc requester timed out"))
          mRelayedIP.clear();
          cancel();
          return;
        }

        if (requester == mPermissionRequester) {
          ZS_LOG_WARNING(Detail, log("permission requester timed out"))
          // clear out any permissions which were hopefully to become installed...
          for (PermissionMap::iterator iter = mPermissions.begin(); iter != mPermissions.end(); ++iter) {
            if ((*iter).second->mInstallingWithRequester == requester) {
              (*iter).second->mInstallingWithRequester.reset(); // will try again soon as "step" executes...
            }
          }

          // we aren't going to treat as fatal but we will try immediately again (perhaps it should be fatal)
          clearPermissionRequester();

          step();
          return;
        }

        // scope: this could be one of the channel bind requests that timed out...
        {
          for(ChannelNumberMap::iterator iter = mChannelNumberMap.begin(); iter != mChannelNumberMap.end(); ++iter) {
            if (requester == (*iter).second->mChannelBindRequester) {
              ZS_LOG_WARNING(Detail, log("channel bind requester timed out"))
              // we found... it timed out, that's unfortunate... we'll try to bind again later when the next timer fires...
              (*iter).second->mChannelBindRequester.reset();
              return;
            }
          }
        }

        // if it reaches here this must be an event for an old requester so ignore it...
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark TURNSocket => IDNSDelegate
      #pragma mark

      //-----------------------------------------------------------------------
      void TURNSocket::onLookupCompleted(IDNSQueryPtr query)
      {
        AutoRecursiveLock lock(mLock);
        if (query == mTURNUDPQuery) {
          step();
          return;
        }

        if (query == mTURNTCPQuery) {
          step();
          return;
        }
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark TURNSocket => ISocketDelegate
      #pragma mark

      //-----------------------------------------------------------------------
      void TURNSocket::onReadReady(SocketPtr socket)
      {
        try
        {
          ITURNSocketDelegatePtr delegate;

          ServerPtr server;

          // scope: obtain the delegate for use later
          {
            AutoRecursiveLock lock(mLock);
            if (isShutdown()) return;

            if (!mActiveServer) {
              for (ServerList::iterator iter = mServers.begin(); iter != mServers.end(); ++iter)
              {
                ServerPtr &serverCompare = (*iter);
                if (serverCompare->mTCPSocket == socket) {
                  server = serverCompare;
                }
              }
            } else {
              if (socket == mActiveServer->mTCPSocket)
                server = mActiveServer;
            }

            if (!server) {
              OPENPEER_SERVICES_WIRE_LOG_WARNING(Detail, log("read ready notification on socket does not match any known servers"))
              return;
            }

            delegate = mDelegate;
          }

          // the TCP socket has more data that can be read...
          while (true)
          {
            // scope: read the turn TCP socket
            {
              AutoRecursiveLock lock(mLock);

              if (isShutdown()) return;
              if (!server->mTCPSocket) {
                OPENPEER_SERVICES_WIRE_LOG_WARNING(Detail, log("TCP socket was closed") + ZS_PARAM("server IP", server->mServerIP.string()))
                return;
              }

              try {
                bool wouldBlock = false;
                size_t bytesRead = 0;
                size_t bytesAvailable = sizeof(server->mReadBuffer) - server->mReadBufferFilledSizeInBytes;

                if (0 != bytesAvailable) {
                  bytesRead = server->mTCPSocket->receive(&(server->mReadBuffer[server->mReadBufferFilledSizeInBytes]), bytesAvailable, &wouldBlock);

                  if (0 == bytesRead) {
                    if (!wouldBlock) {
                      ZS_LOG_WARNING(Detail, log("server closed TURN TCP socket") + ZS_PARAM("server ip", server->mServerIP.string()))
                      onException(socket);
                      return;
                    }
                  }
                }

                if (0 == bytesRead)
                  break;

                server->mReadBufferFilledSizeInBytes += bytesRead;
              } catch(Socket::Exceptions::Unspecified &) {
                ZS_LOG_WARNING(Detail, log("attempt to read TCP TURN socket failed") + ZS_PARAM("server ip", server->mServerIP.string()))
                onException(socket);
                return;
              }
            }

            bool parseAgain = true; // first time this is required
            while (true)
            {
              if (!parseAgain) break;
              parseAgain = false;

              STUNPacketPtr stun;
              boost::shared_array<BYTE> buffer;
              AutoRecycleBuffer autoRecycle(*this, buffer);
              STUNPacket::ParseLookAheadStates ahead = STUNPacket::ParseLookAheadState_InsufficientDataToDeterimine;

              // scope: parse out the buffer
              {
                AutoRecursiveLock lock(mLock);

                size_t consumedBytes = 0;
                ahead = STUNPacket::parseStreamIfSTUN(stun, consumedBytes, &(server->mReadBuffer[0]), server->mReadBufferFilledSizeInBytes, STUNPacket::RFC_5766_TURN, false, "TURNSocket", mID);
                if (0 != consumedBytes) {
                  // the STUN packet is going to have it's parsed pointing into the read buffer which is about to be consumed, fix the pointers now...
                  ZS_THROW_INVALID_ASSUMPTION_IF(!stun)
                  ZS_THROW_INVALID_ASSUMPTION_IF(consumedBytes > OPENPEER_SERVICES_TURNSOCKET_RECYCLE_BUFFER_SIZE)

                  getBuffer(buffer);

                  // make a duplicate of the data buffer
                  memcpy(buffer.get(), &(server->mReadBuffer[0]), consumedBytes);

                  // fix the STUN pointers to now point to the copied buffer instead of the original TCP read buffer...
                  if (stun->mOriginalPacket) {
                    stun->mOriginalPacket = buffer.get() + (stun->mOriginalPacket - &(server->mReadBuffer[0]));
                  }
                  if (stun->mData) {
                    stun->mData = buffer.get() + (stun->mData - &(server->mReadBuffer[0]));
                  }

                  // consume from the read buffer now since it is safe to destroy overtop where the STUN packet was located...
                  consumeBuffer(server, consumedBytes);
                }
              }

              switch (ahead) {
                case STUNPacket::ParseLookAheadState_NotSTUN:                               {
                  IPAddress peer;
                  WORD length = 0;

                  // scope: check to see if this is channel data
                  {
                    AutoRecursiveLock lock(mLock);

                    // this could be data bound to a channel...
                    if (server->mReadBufferFilledSizeInBytes < sizeof(DWORD)) { // insufficient to determine
                      parseAgain = false;
                      continue;
                    }

                    WORD channel = ntohs(((WORD *)(server->mReadBuffer))[0]);
                    length = ntohs(((WORD *)(server->mReadBuffer))[1]);;

                    size_t lengthAsSizeT = length;

                    if ((channel < mLimitChannelToRangeStart) ||
                        (channel > mLimitChannelToRangeEnd) ||
                        (lengthAsSizeT > OPENPEER_SERVICES_TURN_MAX_CHANNEL_DATA_IN_BYTES)) {
                      ZS_LOG_ERROR(Basic, log("socket received bogus data and is being shutdown"))
                      // this socket has bogus data in it...
                      mLastError = TURNSocketError_BogusDataOnSocketReceived;
                      cancel();
                      return;
                    }

                    if (server->mReadBufferFilledSizeInBytes < sizeof(DWORD) + dwordBoundary(((size_t)length))) {
                      // this is channel data, but we have not read enough data to parse it
                      parseAgain = false;
                      continue;
                    }

                    ChannelNumberMap::iterator iter = mChannelNumberMap.find(channel);
                    if (iter == mChannelNumberMap.end()) {
                      // we have to consume the buffer because it is for a channel that no longer exists
                      consumeBuffer(server, sizeof(DWORD) + dwordBoundary(length));
                      parseAgain = true;
                      continue;
                    }

                    peer = (*iter).second->mPeerAddress;

                    getBuffer(buffer);

                    // we now have channel data, parse it out by making a copy to the temporary buffer
                    memcpy(buffer.get(), &(((WORD *)&(server->mReadBuffer[0]))[2]), length);

                    // now we have a copy it is safe to consume from the original buffer
                    consumeBuffer(server, sizeof(DWORD) + dwordBoundary(length));
                  }

                  // this is a legal channel so tell the delegate about the received data...
                  delegate->handleTURNSocketReceivedPacket(mThisWeak.lock(), peer, buffer.get(), length);

                  parseAgain = true;
                  continue;
                }
                case STUNPacket::ParseLookAheadState_InsufficientDataToDeterimine:
                case STUNPacket::ParseLookAheadState_AppearsSTUNButPacketNotFullyAvailable: {
                  parseAgain = false;
                  continue;
                }
                case STUNPacket::ParseLookAheadState_STUNPacket:                            {
                  parseAgain = true;
                  if (ISTUNRequesterManager::handleSTUNPacket(server->mServerIP, stun)) {
                    OPENPEER_SERVICES_WIRE_LOG_TRACE(log("STUN request handled via request manager"))
                    continue;  // if this was handled by the requester manager then nothing more to do
                  }

                  // this was not handled so...
                  switch (stun->mClass) {
                    case STUNPacket::Class_Request:         {
                      ZS_LOG_ERROR(Detail, log("TURN is not expecting incoming requests"))
                      continue;         // TURN clients do not expect to receive incoming requests, so ignore it!
                    }
                    case STUNPacket::Class_Indication:      {
                      if (STUNPacket::Method_Data != stun->mMethod) {
                        ZS_LOG_ERROR(Detail, log("TURN is not expecting indications that are not DATA methods"))
                        continue;   // the only indication we expect to receive as a client is the DATA indication
                      }

                      if (!stun->isLegal(STUNPacket::RFC_5766_TURN)) {
                        ZS_LOG_ERROR(Detail, log("TURN received an illegal STUN request"))
                        continue;  // ignore if this isn't legal for TURN
                      }

                      if (server != mActiveServer) {
                        OPENPEER_SERVICES_WIRE_LOG_WARNING(Detail, log("cannot forward STUN packet when server not promoted to active") + ZS_PARAM("server IP", server->mServerIP.string()))
                        continue;
                      }

                      // send the packet to the delegate which is interested in the data received
                      delegate->handleTURNSocketReceivedPacket(mThisWeak.lock(), stun->mPeerAddressList.front(), stun->mData, stun->mDataLength);
                      continue;
                    }
                    case STUNPacket::Class_Response:
                    case STUNPacket::Class_ErrorResponse:
                    default:                                {
                      // if this was truly a response that was cared about it would have already been handled by the requester manager so ignore the response
                      OPENPEER_SERVICES_WIRE_LOG_WARNING(Detail, log("TURN received a respose (or error response) but it was not handle (likely obsolete)"))
                      continue;
                    }
                  }
                  break;
                }
              }
            }
          }
        } catch (ITURNSocketDelegateProxy::Exceptions::DelegateGone &) {
          OPENPEER_SERVICES_WIRE_LOG_WARNING(Detail, log("delegate gone"))
          cancel();
          return;
        }
      }

      //-----------------------------------------------------------------------
      void TURNSocket::onWriteReady(SocketPtr socket)
      {
        AutoRecursiveLock lock(mLock);

        if (isShutdown()) {
          OPENPEER_SERVICES_WIRE_LOG_WARNING(Detail, log("server notified write ready while shutdown"))
          return;
        }

        ServerPtr server;
        if (!mActiveServer) {
          for (ServerList::iterator iter = mServers.begin(); iter != mServers.end(); ++iter)
          {
            ServerPtr &serverCompare = (*iter);
            if (socket == serverCompare->mTCPSocket) {
              server = serverCompare;
              break;
            }
          }
        } else {
          if (socket == mActiveServer->mTCPSocket)
            server = mActiveServer;
        }

        if (!server) {
          OPENPEER_SERVICES_WIRE_LOG_WARNING(Detail, log("notify write ready did not match any known TCP server connections"))
          return;
        }

        if (!server->mIsConnected) {
          server->mIsConnected = true;
          step();
        }

        sendPacketOverTCPOrDropIfBufferFull(server, NULL, 0); // nothing to add, just send the existing data since we were told there is now room for more...
      }

      //-----------------------------------------------------------------------
      void TURNSocket::onException(SocketPtr socket)
      {
        AutoRecursiveLock lock(mLock);
        if (isShutdown()) {
          ZS_LOG_WARNING(Detail, log("server notified exception while shutdown"))
          return;
        }

        ServerPtr server;
        if (!mActiveServer) {
          for (ServerList::iterator iterServer = mServers.begin(); iterServer != mServers.end();)
          {
            ServerList::iterator current = iterServer;
            ++iterServer;

            ServerPtr &serverCompare = (*current);
            if (socket == serverCompare->mTCPSocket) {
              server = serverCompare;
              mServers.erase(current);
              break;
            }
          }
        } else {
          if (socket == mActiveServer->mTCPSocket)
            server = mActiveServer;
        }

        if (!server) {
          ZS_LOG_WARNING(Detail, log("exception notification did not match any known TCP server connections"))
          return;
        }

        server->mTCPSocket->close();
        server->mTCPSocket.reset();

        if (server == mActiveServer) {
          // the TCP socket was connected but now it is not, so this is fatal...
          mLastError = TURNSocketError_UnexpectedSocketFailure;
          cancel();
          return;
        }

        if (server->mAllocateRequester) {
          server->mAllocateRequester->cancel();
          server->mAllocateRequester.reset();
        }

        step();
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark TURNSocket => ITimer
      #pragma mark

      //-----------------------------------------------------------------------
      void TURNSocket::onTimer(TimerPtr timer)
      {
        AutoRecursiveLock lock(mLock);
        if (isShutdown()) {
          ZS_LOG_WARNING(Detail, log("notified timer event after shutdown"))
          return;
        }

        if (timer == mActivationTimer) {
          ZS_LOG_DEBUG(log("activation timer"))
          step();
          return;
        }

        if (timer == mRefreshTimer) {
          // figure out how much time do we have before the lifetime expires
          DWORD totalSeconds = (mLifetime > (OPENPEER_SERVICES_TURN_RECOMMENDED_REFRESH_BEFORE_LIFETIME_END_IN_SECONDS+30) ? mLifetime - OPENPEER_SERVICES_TURN_RECOMMENDED_REFRESH_BEFORE_LIFETIME_END_IN_SECONDS : mLifetime / 2);
          if (totalSeconds < OPENPEER_SERVICES_TURN_MINIMUM_LIFETIME_FOR_TURN_IN_SECONDS)
            totalSeconds = OPENPEER_SERVICES_TURN_MINIMUM_LIFETIME_FOR_TURN_IN_SECONDS;

          Time current = zsLib::now();

          // if we haven't sent data to the server in a while we should otherwise our firewall socket port could close
          if (mLastSentDataToServer + Seconds(OPENPEER_SERVICES_TURN_MINIMUM_KEEP_ALIVE_FOR_TURN_IN_SECONDS) < current)
            totalSeconds = (totalSeconds > OPENPEER_SERVICES_TURN_MINIMUM_KEEP_ALIVE_FOR_TURN_IN_SECONDS ? OPENPEER_SERVICES_TURN_MINIMUM_KEEP_ALIVE_FOR_TURN_IN_SECONDS : totalSeconds);

          if (mLastRefreshTimerWasSentAt + Seconds(totalSeconds) > current) {
            // we don't need to refresh yet because we sent data to the server recently and the lifetime hasn't expired yet...
            return;
          }

          refreshNow();
          return;
        }

        if (timer == mPermissionTimer) {
          requestPermissionsNow();
          step();
          return;
        }

        if (timer == mDeallocTimer) {
          ZS_LOG_DEBUG(log("on dealloc timer"))
          cancel();
          return;
        }

        // check if it is one of the channel timers...
        for (ChannelNumberMap::iterator iter = mChannelNumberMap.begin(); iter != mChannelNumberMap.end(); ++iter) {
          ChannelInfoPtr info = (*iter).second;
          if (timer == info->mRefreshTimer) {
            if (info->mChannelBindRequester) return;  // already have an outstanding request so do nothing...

            ZS_LOG_DEBUG(log("channel bind starting now"))

            ZS_THROW_INVALID_ASSUMPTION_IF(!mActiveServer)

            STUNPacketPtr newRequest = STUNPacket::createRequest(STUNPacket::Method_ChannelBind);
            fix(newRequest);
            newRequest->mUsername = mUsername;
            newRequest->mPassword = mPassword;
            newRequest->mRealm = mRealm;
            newRequest->mNonce = mNonce;
            newRequest->mCredentialMechanism = STUNPacket::CredentialMechanisms_LongTerm;
            newRequest->mChannelNumber = info->mChannelNumber;
            newRequest->mPeerAddressList.push_back(info->mPeerAddress);
            info->mChannelBindRequester = ISTUNRequester::create(getAssociatedMessageQueue(), mThisWeak.lock(), mActiveServer->mServerIP, newRequest, STUNPacket::RFC_5766_TURN);
            return;
          }
        }
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark TURNSocket => IBackgroundingDelegate
      #pragma mark

      //-----------------------------------------------------------------------
      void TURNSocket::onBackgroundingGoingToBackground(
                                                        IBackgroundingSubscriptionPtr subscription,
                                                        IBackgroundingNotifierPtr notifier
                                                        )
      {
        AutoRecursiveLock lock(mLock);

        ZS_LOG_DEBUG(log("going to background thus will attempt to refresh TURN socket now to ensure we have the maximum lifetime before the TURN server deletes this client's bindings"))

        if (mPermissionTimer) {
          requestPermissionsNow();
        }

        refreshNow();

        mBackgroundingNotifier = notifier;

        clearBackgroundingNotifierIfPossible();
      }

      //-----------------------------------------------------------------------
      void TURNSocket::onBackgroundingGoingToBackgroundNow(IBackgroundingSubscriptionPtr subscription)
      {
        AutoRecursiveLock lock(mLock);

        ZS_LOG_DEBUG(log("going to the background immediately thus cancel any pending refresh requester"))

        clearRefreshRequester();
        clearPermissionRequester();

        mBackgroundingNotifier.reset();
      }

      //-----------------------------------------------------------------------
      void TURNSocket::onBackgroundingReturningFromBackground(IBackgroundingSubscriptionPtr subscription)
      {
        AutoRecursiveLock lock(mLock);

        ZS_LOG_DEBUG("returning from background")

        if (mActiveServer) {
          if (mActiveServer->mTCPSocket) {
            ZS_LOG_DEBUG(log("returning from background and will force active TCP socket to check if it can be read by simulating a read-ready"))
            ISocketDelegateProxy::create(mThisWeak.lock())->onReadReady(mActiveServer->mTCPSocket);
          }
        } else {
          for (ServerList::iterator iter = mServers.begin(); iter != mServers.end(); ++iter)
          {
            ServerPtr &server = (*iter);
            if (!server->mTCPSocket) continue;

            ZS_LOG_DEBUG(log("returning from background and will force TCP socket to check if it can be read by simulating a read-ready") + ZS_PARAM("server ip", server->mServerIP.string()))
            ISocketDelegateProxy::create(mThisWeak.lock())->onReadReady(server->mTCPSocket);
          }
        }

        // force a refresh of the TURN socket immediately
        refreshNow();

        if (mPermissionTimer) {
          requestPermissionsNow();
        }

        // perform routine maintanence
        step();
      }

      //-----------------------------------------------------------------------
      void TURNSocket::onBackgroundingApplicationWillQuit(IBackgroundingSubscriptionPtr subscription)
      {
        ZS_LOG_DEBUG("application will quit")
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark TURNSocket => (internal)
      #pragma mark

      //-----------------------------------------------------------------------
      Log::Params TURNSocket::log(const char *message) const
      {
        ElementPtr objectEl = Element::create("TURNSocket");
        IHelper::debugAppend(objectEl, "id", mID);
        return Log::Params(message, objectEl);
      }

      //-----------------------------------------------------------------------
      Log::Params TURNSocket::debug(const char *message) const
      {
        return Log::Params(message, toDebug());
      }

      //-----------------------------------------------------------------------
      void TURNSocket::fix(STUNPacketPtr stun) const
      {
        stun->mLogObject = "TURNSocket";
        stun->mLogObjectID = mID;
      }

      //-----------------------------------------------------------------------
      ElementPtr TURNSocket::toDebug() const
      {
        AutoRecursiveLock lock(mLock);
        ElementPtr resultEl = Element::create("TURNSocket");

        IHelper::debugAppend(resultEl, "id", mID);

        IHelper::debugAppend(resultEl, "current state", toString(mCurrentState));
        IHelper::debugAppend(resultEl, "last error", toString(mLastError));

        IHelper::debugAppend(resultEl, "backgrounding subscription", (bool)mBackgroundingSubscription);
        IHelper::debugAppend(resultEl, "backgrounding notifier", (bool)mBackgroundingNotifier);

        IHelper::debugAppend(resultEl, "limit channel range (start)", mLimitChannelToRangeStart);
        IHelper::debugAppend(resultEl, "limit channel range (end)", mLimitChannelToRangeEnd);
        IHelper::debugAppend(resultEl, "delegate", (bool)mDelegate);
        IHelper::debugAppend(resultEl, "server name", mServerName);
        IHelper::debugAppend(resultEl, "username", mUsername);
        IHelper::debugAppend(resultEl, "password", mPassword);
        IHelper::debugAppend(resultEl, "realm", mRealm);
        IHelper::debugAppend(resultEl, "nonce", mNonce);
        IHelper::debugAppend(resultEl, "udp dns query", (bool)mTURNUDPQuery);
        IHelper::debugAppend(resultEl, "tcp dns query", (bool)mTURNTCPQuery);
        IHelper::debugAppend(resultEl, "udp dns srv records", mTURNUDPSRVResult ? mTURNUDPSRVResult->mRecords.size() : 0);
        IHelper::debugAppend(resultEl, "tcp dns srv records", mTURNTCPSRVResult ? mTURNTCPSRVResult->mRecords.size() : 0);
        IHelper::debugAppend(resultEl, "use channel binding", mUseChannelBinding);
        IHelper::debugAppend(resultEl, "allocated response IP", mAllocateResponseIP.string());
        IHelper::debugAppend(resultEl, "relayed IP", mRelayedIP.string());
        IHelper::debugAppend(resultEl, "reflected IP", mReflectedIP.string());
        if (mActiveServer) {
          ElementPtr activeServerEl = Element::create("active server");
          IHelper::debugAppend(activeServerEl, "is udp", mActiveServer->mIsUDP);
          IHelper::debugAppend(activeServerEl, "server ip", mActiveServer->mServerIP.string());
          IHelper::debugAppend(activeServerEl, "tcp socket", (bool)mActiveServer->mTCPSocket);
          IHelper::debugAppend(activeServerEl, "connected", mActiveServer->mIsConnected);
          IHelper::debugAppend(activeServerEl, "write ready", mActiveServer->mInformedWriteReady);
          IHelper::debugAppend(activeServerEl, "activate after", mActiveServer->mActivateAfter);
          IHelper::debugAppend(activeServerEl, "allocate requestor", (bool)mActiveServer->mAllocateRequester);
          IHelper::debugAppend(activeServerEl, "read buffer fill size", mActiveServer->mReadBufferFilledSizeInBytes);
          IHelper::debugAppend(activeServerEl, "write buffer fill size", mActiveServer->mWriteBufferFilledSizeInBytes);
          IHelper::debugAppend(resultEl, activeServerEl);
        }
        IHelper::debugAppend(resultEl, "lifetime", mLifetime);
        IHelper::debugAppend(resultEl, "refresh requester", (bool)mRefreshRequester);
        IHelper::debugAppend(resultEl, "refresh timer", (bool)mRefreshTimer);
        IHelper::debugAppend(resultEl, "last send data to server", mLastSentDataToServer);
        IHelper::debugAppend(resultEl, "last refreash timer was sent", mLastRefreshTimerWasSentAt);
        IHelper::debugAppend(resultEl, "deallocate requester", (bool)mDeallocateRequester);
        IHelper::debugAppend(resultEl, "deallocate timer", (bool)mDeallocTimer);
        IHelper::debugAppend(resultEl, "servers", mServers.size());
        IHelper::debugAppend(resultEl, "activation timer", (bool)mActivationTimer);
        IHelper::debugAppend(resultEl, "permissions", mPermissions.size());
        IHelper::debugAppend(resultEl, "permission timer", (bool)mPermissionTimer);
        IHelper::debugAppend(resultEl, "permission requester", (bool)mPermissionRequester);
        IHelper::debugAppend(resultEl, "permission max capacity", mPermissionRequesterMaxCapacity);
        IHelper::debugAppend(resultEl, "channel IP map", mChannelIPMap.size());
        IHelper::debugAppend(resultEl, "channel number map", mChannelNumberMap.size());
        IHelper::debugAppend(resultEl, "recycle buffers", mRecycledBuffers.size());

        return resultEl;
      }

      //-----------------------------------------------------------------------
      static bool hasAddedBefore(
                                 const IPAddressList &alreadyAdded,
                                 const IPAddress &server
                                 )
      {
        return alreadyAdded.end() != find(alreadyAdded.begin(), alreadyAdded.end(), server);
      }

      //-----------------------------------------------------------------------
      IPAddress TURNSocket::stepGetNextServer(
                                              IPAddressList &previouslyAdded,
                                              SRVResultPtr &srv
                                              )
      {
        IPAddress result;

        // we don't have UDP server IP, try to obtain...
        while (true) {
          result.clear();

          bool found = IDNS::extractNextIP(srv, result);
          if (!found) {
            ZS_LOG_DEBUG(log("no more servers found") + ZS_PARAM("server", (srv ? srv->mName : mServerName)))

            // we failed to discover any UDP server that works
            return IPAddress();
          }

          if (result.isAddressEmpty()) continue;
          if (result.isPortEmpty()) continue;

          if (hasAddedBefore(previouslyAdded, result)) {
            continue;
          }

          ZS_LOG_DETAIL(log("found server") + ZS_PARAM("server", (srv ? srv->mName : mServerName)) + ZS_PARAM("ip", result.string()))

          // we now know the next server to try
          previouslyAdded.push_back(result);
          break;
        }

        return result;
      }

      //-----------------------------------------------------------------------
      bool TURNSocket::stepPrepareServers()
      {
        if ((mServers.size() > 0) ||
            (mActiveServer)) {
          ZS_LOG_TRACE(log("servers are already prepared"))
          return true;
        }

        IPAddressList previouslyContactedUDPServers;
        IPAddressList previouslyContactedTCPServers;
        bool udpExhausted = mForceTURNUseTCP;   // if true then no UDP server will ever be used
        bool tcpExhausted = mForceTURNUseUDP;   // if true then no TCP server will ever be used

        Time activateAfter = zsLib::now();

        ULONG count = 0;
        while ((!udpExhausted) &&
               (!tcpExhausted))
        {
          bool toggle = (0 == count ? true : ((count % 2) == 1)); // true, true, false, true, false, true, false, ...
          ++count;

          IPAddressList &previousList = (toggle ? previouslyContactedUDPServers : previouslyContactedTCPServers);
          bool &exhausted = (toggle ? udpExhausted : tcpExhausted);
          SRVResultPtr &srv = (toggle ? mTURNUDPSRVResult : mTURNTCPSRVResult);

          if (exhausted) continue;

          IPAddress result = stepGetNextServer(previousList, srv);
          if (result.isAddressEmpty()) {
            exhausted = true;
            continue;
          }

          ServerPtr server = Server::create();
          server->mIsUDP = toggle;
          server->mServerIP = result;
          server->mActivateAfter = activateAfter;

          activateAfter += Seconds(OPENPEER_SERVICES_TURN_ACTIVATE_NEXT_SERVER_IN_SECONDS);

          mServers.push_back(server);
        }

        return mServers.size() > 0;
      }

      //-----------------------------------------------------------------------
      void TURNSocket::step()
      {
        if ((isShutdown()) ||
            (isShuttingDown())) {
          cancel();
          return;
        }

        if (!mTURNUDPSRVResult) {
          if (!mTURNUDPQuery) {
            ZS_LOG_DEBUG(log("performing _turn._udp SRV lookup") + ZS_PARAM("server", mServerName))
            mTURNUDPQuery = IDNS::lookupSRV(mThisWeak.lock(), mServerName, "turn", "udp", 3478);
          }

          if (!mTURNUDPQuery->isComplete())
            return;

          mTURNUDPSRVResult = mTURNUDPQuery->getSRV();
        }

        if (!mTURNTCPSRVResult) {
          if (!mTURNTCPQuery) {
            ZS_LOG_DEBUG(log("performing _turn._tcp SRV lookup") + ZS_PARAM("server", mServerName))
            mTURNTCPQuery = IDNS::lookupSRV(mThisWeak.lock(), mServerName, "turn", "tcp", 3478);
          }

          if (!mTURNTCPQuery->isComplete())
            return;

          mTURNTCPSRVResult = mTURNTCPQuery->getSRV();
        }

        if ((!mTURNUDPSRVResult) &&
            (!mTURNTCPSRVResult)) {
          mLastError = TURNSocketError_DNSLookupFailure;
          cancel();
          return;
        }

        if (!stepPrepareServers()) {
          ZS_LOG_WARNING(Detail, log("failed to prepare servers"))
          mLastError = TURNSocketError_FailedToConnectToAnyServer;
          cancel();
          return;
        }

        if (mServers.size() > 0) {
          if (!mActivationTimer) {
            mActivationTimer = Timer::create(mThisWeak.lock(), Seconds(1));
          }
        }

        Time tick = zsLib::now();

        // see if there are any servers to be activated...
        if (mRelayedIP.isAddressEmpty()) {
          // try to activate the next server on the list...
          for (ServerList::iterator iter = mServers.begin(); iter != mServers.end(); ++iter)
          {
            ServerPtr &server = (*iter);

            if (server->mActivateAfter > tick) {
              ZS_LOG_TRACE(log("next server can't activate until later") + ZS_PARAM("when", server->mActivateAfter))
              break;
            }

            if (!server->mIsUDP) {
              if (!server->mTCPSocket) {
                ZS_LOG_DEBUG(log("creating socket for TCP") + ZS_PARAM("server IP", server->mServerIP.string()))
                server->mTCPSocket = Socket::createTCP();
                server->mTCPSocket->setBlocking(false);
                try {
#ifndef __QNX__
                  server->mTCPSocket->setOptionFlag(Socket::SetOptionFlag::IgnoreSigPipe, true);
#endif //ndef __QNX__
                } catch(Socket::Exceptions::UnsupportedSocketOption &) {
                }

                try {
                  bool wouldBlock = false;
                  server->mTCPSocket->connect(server->mServerIP, &wouldBlock);
                } catch(Socket::Exceptions::Unspecified &) {
                  mLastError = TURNSocketError_UnexpectedSocketFailure;
                  cancel();
                  return;
                }
                server->mTCPSocket->setDelegate(mThisWeak.lock());  // set delegate must happen after the connect request
              }

              if (!server->mIsConnected) {
                ZS_LOG_TRACE(log("waiting for TCP socket to connect") + ZS_PARAM("server IP", server->mServerIP.string()))
                continue;
              }
            }

            if (server->mAllocateRequester) {
              ZS_LOG_TRACE(log("allocate requester already activated") + ZS_PARAM("server IP", server->mServerIP.string()))
              continue;
            }

            ZS_LOG_DETAIL(log("creating alloc request") + ZS_PARAM("server IP", server->mServerIP.string()) + ZS_PARAM("is UDP", server->mIsUDP))

            mLifetime = OPENPEER_SERVICES_TURN_RECOMMENDED_LIFETIME_IN_SECONDS;

            // we don't have an allocate request - form one now
            STUNPacketPtr allocRequest = STUNPacket::createRequest(STUNPacket::Method_Allocate);
            fix(allocRequest);
            allocRequest->mLifetimeIncluded = true;
            allocRequest->mLifetime = mLifetime;
            allocRequest->mRequestedTransport = STUNPacket::Protocol_UDP;
            allocRequest->mDontFragmentIncluded = true;
            server->mAllocateRequester = ISTUNRequester::create(getAssociatedMessageQueue(), mThisWeak.lock(), server->mServerIP, allocRequest, STUNPacket::RFC_5766_TURN);
          }
        }

        // check to see if we know the relayed IP... if not we better get it
        if (mRelayedIP.isAddressEmpty()) {
          ZS_LOG_TRACE(log("waiting for a TURN allocation to complete"))
          return;
        }

        // we know the relay address... make sure we have a timer setup to do refresh before the expiry
        if (!mRefreshTimer) {
          mRefreshTimer = Timer::create(mThisWeak.lock(), Seconds(10));
        }

        // scope: we need to make sure all permissions are installed
        {
          bool found = false;
          for (PermissionMap::iterator iter = mPermissions.begin(); iter != mPermissions.end(); ++iter) {
            PermissionPtr permission = (*iter).second;
            if ((!(permission->mInstalled)) &&
                (!(permission->mInstallingWithRequester))) {
              found = true;
              break;
            }
          }

          if (found) {
            ZS_LOG_DEBUG(log("will create permisson request now"))
            requestPermissionsNow();
          }
        }

        // we need to refresh permissions every 4 minutes
        if (!mPermissionTimer) {
          mPermissionTimer = Timer::create(mThisWeak.lock(), Seconds(OPENPEER_SERVICES_TURN_PERMISSION_RETRY_IN_SECONDS));  // refresh permissions every 4 minutes
        }

        // finally we need to make sure all channels are created imemdiately
        refreshChannels();
      }

      //-----------------------------------------------------------------------
      void TURNSocket::cancel()
      {
        AutoRecursiveLock lock(mLock);    // just in case

        if (isShutdown()) {
          ZS_LOG_DEBUG(log("already cancelled"))
          return; // already shutdown...
        }

        ZS_LOG_DEBUG(log("cancel called"))

        setState(ITURNSocket::TURNSocketState_ShuttingDown);

        if (!mGracefulShutdownReference) mGracefulShutdownReference = mThisWeak.lock();                        // prevent object from being destroyed before the graceful shutdown...

        if (mBackgroundingSubscription) {
          mBackgroundingSubscription->cancel();
          mBackgroundingSubscription.reset();
        }

        mServers.clear();

        clearRefreshRequester();
        clearPermissionRequester();

        mPermissions.clear();
        for (ChannelIPMap::iterator iter = mChannelIPMap.begin(); iter != mChannelIPMap.end(); ++iter) {
          ChannelInfoPtr info = (*iter).second;
          if (info->mRefreshTimer) {
            info->mRefreshTimer->cancel();
            info->mRefreshTimer.reset();
          }
          if (info->mChannelBindRequester) {
            info->mChannelBindRequester->cancel();
            info->mChannelBindRequester.reset();
          }
        }
        mChannelIPMap.clear();
        mChannelNumberMap.clear();

        if (mActivationTimer) {
          mActivationTimer->cancel();
          mActivationTimer.reset();
        }
        if (mRefreshTimer) {
          mRefreshTimer->cancel();
          mRefreshTimer.reset();
        }
        if (mPermissionTimer) {
          mPermissionTimer->cancel();
          mPermissionTimer.reset();
        }

        if (mTURNUDPQuery) {
          mTURNUDPQuery->cancel();
          mTURNUDPQuery.reset();
        }
        if (mTURNTCPQuery) {
          mTURNTCPQuery->cancel();
          mTURNTCPQuery.reset();
        }

        mTURNUDPSRVResult.reset();
        mTURNTCPSRVResult.reset();

        if (mGracefulShutdownReference) {

          if (!mRelayedIP.isAddressEmpty()) {                                   // if we have a relay address then we must do a proper shutdown

            ITURNSocketDelegatePtr originalDelegate = ITURNSocketDelegateProxy::original(mDelegate);

            if ((!mDeallocateRequester) &&
                (originalDelegate)) {
              ZS_LOG_DETAIL(log("graceful shutdown started"))

              // we need to shutdown gracefully... start the process now...
              STUNPacketPtr deallocRequest = STUNPacket::createRequest(STUNPacket::Method_Refresh);
              fix(deallocRequest);
              deallocRequest->mUsername = mUsername;
              deallocRequest->mPassword = mPassword;
              deallocRequest->mRealm = mRealm;
              deallocRequest->mNonce = mNonce;
              deallocRequest->mLifetimeIncluded = true;
              deallocRequest->mLifetime = 0;
              deallocRequest->mCredentialMechanism = STUNPacket::CredentialMechanisms_LongTerm;
              mDeallocateRequester = ISTUNRequester::create(getAssociatedMessageQueue(), mThisWeak.lock(), mActiveServer->mServerIP, deallocRequest, STUNPacket::RFC_5766_TURN);

              if (!mDeallocTimer) {
                mDeallocTimer = Timer::create(mGracefulShutdownReference, Seconds(1));
              }
            }

            if (!originalDelegate) {
              clearDeallocateRequester();
            }
          }

          // are all graceful shutdowns complete?
          if (mDeallocateRequester) {
            if (!mDeallocateRequester->isComplete()) return;
          }
        }

        setState(ITURNSocket::TURNSocketState_Shutdown);

        ZS_LOG_DETAIL(log("performing final cleanup"))

        mBackgroundingNotifier.reset();

        if (mDeallocTimer) {
          mDeallocTimer->cancel();
          mDeallocTimer.reset();
        }

        mGracefulShutdownReference.reset();
        mDelegate.reset();

        clearDeallocateRequester();

        mActiveServer.reset();

        mRelayedIP.clear();
        mReflectedIP.clear();
      }

      //-----------------------------------------------------------------------
      void TURNSocket::setState(TURNSocketStates newState)
      {
        if (mCurrentState == newState) return;

        ZS_LOG_DETAIL(log("state changed") + ZS_PARAM("old state", toString(mCurrentState)) + ZS_PARAM("new state", toString(newState)) + ZS_PARAM("error", toString(mLastError)))
        mCurrentState = newState;

        if (!mDelegate) return;

        TURNSocketPtr pThis = mThisWeak.lock();

        if (pThis) {
          try {
            mDelegate->onTURNSocketStateChanged(pThis, mCurrentState);
          } catch (ITURNSocketDelegateProxy::Exceptions::DelegateGone &) {
          }
        }
      }

      //-----------------------------------------------------------------------
      void TURNSocket::consumeBuffer(
                                     ServerPtr &server,
                                     size_t consumeSizeInBytes
                                     )
      {
        size_t remaining = (server->mReadBufferFilledSizeInBytes > consumeSizeInBytes ? server->mReadBufferFilledSizeInBytes - consumeSizeInBytes : 0);
        if (0 == remaining) {
          server->mReadBufferFilledSizeInBytes = 0;
          return;
        }

        memcpy(&(server->mReadBuffer[0]), &(server->mReadBuffer[consumeSizeInBytes]), remaining);
        server->mReadBufferFilledSizeInBytes = remaining;
      }

      //-----------------------------------------------------------------------
      bool TURNSocket::handleAllocateRequester(
                                               ISTUNRequesterPtr requester,
                                               IPAddress fromIPAddress,
                                               STUNPacketPtr response
                                               )
      {
        ServerPtr server;
        ServerList::iterator found = mServers.end();

        STUNPacketPtr request = requester->getRequest();
        if (STUNPacket::Method_Allocate != request->mMethod) {
          ZS_LOG_INSANE(log("not an allocation request"))
          return false;
        }

        for (ServerList::iterator iter = mServers.begin(); iter != mServers.end(); ++iter)
        {
          ServerPtr &serverCompare = (*iter);
          if (requester == serverCompare->mAllocateRequester) {
            server = serverCompare;
            found = iter;
          }
        }

        if (!server) {
          ZS_LOG_WARNING(Detail, log("allocation requester does not match any known servers"))
          return false;
        }

        server->mAllocateRequester = handleAuthorizationErrors(requester, response);
        if (server->mAllocateRequester) {
          ZS_LOG_DEBUG(log("reissued allocate requester") + ZS_PARAM("server IP", server->mServerIP.string()))
          return true;
        }

        if ((0 != response->mErrorCode) ||
            (STUNPacket::Class_ErrorResponse == response->mClass)) {

          ZS_LOG_WARNING(Detail, log("alloc request failed") + ZS_PARAM("username", mUsername) + ZS_PARAM("password", mPassword) + ZS_PARAM("server IP", server->mServerIP.string()))

          bool tryDifferentServer = true;

          // this request failed - why?
          switch (response->mErrorCode) {
            case STUNPacket::ErrorCode_UnknownAttribute:              {
              if (response->hasUnknownAttribute(STUNPacket::Attribute_DontFragment)) {
                ZS_LOG_WARNING(Detail, log("alloc failed thus attempting again without DONT_FRAGMENT attribute") + ZS_PARAM("server IP", server->mServerIP.string()))

                // did not understand the don't fragment, try again without it
                STUNPacketPtr newRequest = (requester->getRequest())->clone(true);
                newRequest->mDontFragmentIncluded = false;
                server->mAllocateRequester = ISTUNRequester::create(getAssociatedMessageQueue(), mThisWeak.lock(), server->mServerIP, newRequest, STUNPacket::RFC_5766_TURN);
                return true;
              }
              break;
            }
            case STUNPacket::ErrorCode_Unauthorized:                  break;
            case STUNPacket::ErrorCode_UnsupportedTransportProtocol:  break;
            default:                                                  break;
          }

          // clear our current server so we can try a new one
          mServers.erase(found);

          if (!tryDifferentServer) {
            mServers.clear();
          }
          (IWakeDelegateProxy::create(mThisWeak.lock()))->onWake();
          return true;
        }

        // if this was a proper successful response then it should be signed with integrity
        if (!response->isValidMessageIntegrity(mPassword, mUsername, mRealm)) {
          ZS_LOG_ERROR(Detail, log("alloc response did not pass integrity check") + ZS_PARAM("server IP", server->mServerIP.string()))
          return false; // this didn't have valid message integrity so it's not a valid response
        }

        if (response->hasAttribute(STUNPacket::Attribute_Lifetime)) {
          mLifetime = response->mLifetime;
        }

        mAllocateResponseIP = fromIPAddress;
        mRelayedIP = response->mRelayedAddress;
        mReflectedIP = response->mMappedAddress;
        mActiveServer = server;
        mServers.clear();
        if (mActivationTimer) {
          mActivationTimer->cancel();
          mActivationTimer.reset();
        }

        ZS_LOG_DETAIL(log("alloc request completed") + ZS_PARAM("relayed ip", mRelayedIP.string()) + ZS_PARAM("reflected", mReflectedIP.string()) + ZS_PARAM("username", mUsername) + ZS_PARAM("password", mPassword) + ZS_PARAM("server IP", server->mServerIP.string()))

        setState(TURNSocketState_Ready);

        informWriteReady();

        (IWakeDelegateProxy::create(mThisWeak.lock()))->onWake();
        return true;
      }

      //-----------------------------------------------------------------------
      bool TURNSocket::handleDeallocRequester(
                                              ISTUNRequesterPtr requester,
                                              STUNPacketPtr response
                                              )
      {
        if (requester != mDeallocateRequester) return false;

        ISTUNRequesterPtr replacementRequester = handleAuthorizationErrors(requester, response);
        if (replacementRequester) {
          mDeallocateRequester = replacementRequester;
          ZS_LOG_TRACE(log("replacement dealloc requester created") + ZS_PARAM("requester", mDeallocateRequester->getID()))
          return true;
        }

        clearDeallocateRequester();

        ZS_LOG_DETAIL(log("dealloc request completed"))

        // if this was a proper successful response then it should be signed with integrity (but they could just fake an error back and cause it to cancel so why bother verifying this...
//        if (!response->isValidMessageIntegrity(mPassword, mUsername, mRealm))
//          return false; // this didn't have valid message integrity so it's not a valid response

        mRelayedIP.clear();

        // can't simply call cancel because the "dealloc" requester will return false that it's complete until we return "true" from this method
        (IWakeDelegateProxy::create(mThisWeak.lock()))->onWake();
        return true;
      }

      //-----------------------------------------------------------------------
      bool TURNSocket::handleRefreshRequester(
                                              ISTUNRequesterPtr requester,
                                              STUNPacketPtr response
                                              )
      {
        if (requester != mRefreshRequester) return false;

        mRefreshRequester = handleAuthorizationErrors(requester, response);

        if (mRefreshRequester) {
          ZS_LOG_TRACE(log("replacement refresh requester created") + ZS_PARAM("requester", mRefreshRequester->getID()))
          return true;
        }

        clearBackgroundingNotifierIfPossible();

        if ((0 != response->mErrorCode) ||
            (STUNPacket::Class_ErrorResponse == response->mClass)) {

          ZS_LOG_WARNING(Detail, log("refresh requester failed because of error thus shutting down") + ZS_PARAM("error", response->mErrorCode) + ZS_PARAM("reason", response->mReason))

          // this is a problem if we can't refresh... cancel the connection...
          cancel();
          return true;
        }

        // if this was a proper successful response then it should be signed with integrity
        if (!response->isValidMessageIntegrity(mPassword, mUsername, mRealm)) {
          ZS_LOG_ERROR(Detail, log("refresh response did not pass integrity check"))
          return false; // this didn't have valid message integrity so it's not a valid response
        }

        ZS_LOG_DEBUG(log("refresh requester completed"))
        if (response->hasAttribute(STUNPacket::Attribute_Lifetime)) {
          mLifetime = response->mLifetime;
        }

        return true;
      }

      //-----------------------------------------------------------------------
      bool TURNSocket::handlePermissionRequester(
                                                 ISTUNRequesterPtr requester,
                                                 STUNPacketPtr response
                                                 )
      {
        Permission::PendingDataList tempList;

        // scope; we can't be in the context of a lock when we call that send routine
        {
          AutoRecursiveLock lock(mLock);
          if (requester != mPermissionRequester) return false;

          ZS_THROW_INVALID_ASSUMPTION_IF(!mActiveServer)

          mPermissionRequester = handleAuthorizationErrors(requester, response);
          if (mPermissionRequester) {
            ZS_LOG_TRACE(log("replacement permission requester created") + ZS_PARAM("requester", mPermissionRequester->getID()))

            // failed to install permission... but we are trying again
            for (PermissionMap::iterator iter = mPermissions.begin(); iter != mPermissions.end(); ++iter) {
              if ((*iter).second->mInstallingWithRequester == requester) {
                (*iter).second->mInstallingWithRequester = mPermissionRequester;
              }
            }
            return true;
          }

          if ((0 != response->mErrorCode) ||
              (STUNPacket::Class_ErrorResponse == response->mClass)) {

            // we can't install persmission... oh well... try again later...
            if (STUNPacket::ErrorCode_InsufficientCapacity == response->mErrorCode) {
              STUNPacketPtr packet = requester->getRequest();
              if (packet) {
                if (packet->mPeerAddressList.size() > 1) {
                  mPermissionRequesterMaxCapacity = packet->mPeerAddressList.size() - 1;
                  IWakeDelegateProxy::create(mThisWeak.lock())->onWake();
                }
              }
            }

            // failed to install permission...
            for (PermissionMap::iterator iter = mPermissions.begin(); iter != mPermissions.end(); ++iter) {
              if ((*iter).second->mInstallingWithRequester == requester) {
                ZS_LOG_WARNING(Detail, log("permission requester failed because of error") + ZS_PARAM("error", response->mErrorCode) + ZS_PARAM("reason", response->mReason))
                (*iter).second->mInstallingWithRequester.reset();
              }
            }

            return true;
          }

          // if this was a proper successful response then it should be signed with integrity
          if (!response->isValidMessageIntegrity(mPassword, mUsername, mRealm)) {
            ZS_LOG_ERROR(Detail, log("permission response did not pass integrity check"))
            return false; // this didn't have valid message integrity so it's not a valid response
          }

          ZS_LOG_DEBUG(log("permission requester completed"))

          for (PermissionMap::iterator iter = mPermissions.begin(); iter != mPermissions.end(); ++iter) {
            if ((*iter).second->mInstallingWithRequester == requester) {

              ZS_LOG_DEBUG(log("permission installed"))

              (*iter).second->mInstalled = true;
              (*iter).second->mInstallingWithRequester.reset();

              // we have to deliver any pending packets now...
              for (Permission::PendingDataList::iterator pendingIter = (*iter).second->mPendingData.begin(); pendingIter != (*iter).second->mPendingData.end(); ++pendingIter) {
                // this is in a format ready to go (post STUN encoded already)...
                tempList.push_back(*pendingIter);
              }
              // clear out all the pending data, we have sent what we possibly can...
              (*iter).second->mPendingData.clear();
            }
          }
        }

        // now we can send out those permissions while not in the context of a lock
        for (Permission::PendingDataList::iterator iter = tempList.begin(); iter != tempList.end(); ++iter) {
          sendPacketOrDopPacketIfBufferFull(mActiveServer, *(*iter), (*iter)->SizeInBytes());
        }
        return true;
      }

      //-----------------------------------------------------------------------
      bool TURNSocket::handleChannelRequester(
                                              ISTUNRequesterPtr requester,
                                              STUNPacketPtr response
                                              )
      {
        ChannelInfoPtr found;
        // find the requester to which this response belongs...
        for(ChannelNumberMap::iterator iter = mChannelNumberMap.begin(); iter != mChannelNumberMap.end(); ++iter) {
          if (requester == (*iter).second->mChannelBindRequester) {
            found = (*iter).second;
            break;
          }
        }
        if (!found) return false;

        // we found - try to handle basic authorization issues...
        found->mChannelBindRequester = handleAuthorizationErrors(requester, response);
        if (found->mChannelBindRequester) {
          ZS_LOG_TRACE(log("replacement channel bind requester created") + ZS_PARAM("requester", found->mChannelBindRequester->getID()))
          return true;
        }

        if ((0 != response->mErrorCode) ||
            (STUNPacket::Class_ErrorResponse == response->mClass)) {

          ZS_LOG_WARNING(Detail, log("channel bind requester failed because of error") + ZS_PARAM("error", response->mErrorCode) + ZS_PARAM("reason", response->mReason))

          // we can't install channel binding... oh well... try again later...
          found->mBound = false;
          return true;
        }

        // if this was a proper successful response then it should be signed with integrity
        if (!response->isValidMessageIntegrity(mPassword, mUsername, mRealm)) {
          ZS_LOG_ERROR(Detail, log("channel bind response did not pass integrity check"))
          return false; // this didn't have valid message integrity so it's not a valid response
        }

        ZS_LOG_DETAIL(log("channel bind request completed") + ZS_PARAM("channel", found->mChannelNumber))

        // the request completed just fine...
        found->mBound = true;
        return true;
      }

      //-----------------------------------------------------------------------
      void TURNSocket::requestPermissionsNow()
      {
        // we don't care of the previous permissions succeeded or not, we are going to send one right now
        clearPermissionRequester();

        // scope: clear our permissions that have not seen data sent out in a long time
        {
          bool found = false;

          Time time = zsLib::now();
          for (PermissionMap::iterator permIter = mPermissions.begin(); permIter != mPermissions.end(); )
          {
            PermissionMap::iterator current = permIter;
            ++permIter;

            if (time > ((*current).second->mLastSentDataAt + Seconds(OPENPEER_SERVICES_TURN_REMOVE_PERMISSION_IF_NO_DATA_IN_SECONDS))) {
              mPermissions.erase(current);
            } else
              found = true;
          }

          if (!found)
            return;
        }

        ZS_LOG_DEBUG(log("starting permission requester now"))

        while ((mPermissions.size() > mPermissionRequesterMaxCapacity) &&
               (0 != mPermissionRequesterMaxCapacity))
        {
          Time oldestTime = zsLib::now();
          PermissionMap::iterator oldestFound = mPermissions.end();

          for (PermissionMap::iterator iter = mPermissions.begin(); iter != mPermissions.end(); ++iter) {
            PermissionPtr permission = (*iter).second;
            if (permission->mLastSentDataAt < oldestTime) {
              oldestTime = permission->mLastSentDataAt;
              oldestFound = iter;
            }
          }

          if (oldestFound == mPermissions.end()) {
            break;
          }

          mPermissions.erase(oldestFound);
        }

        STUNPacketPtr permissionRequest = STUNPacket::createRequest(STUNPacket::Method_CreatePermission);
        fix(permissionRequest);

        // scope: add permissions for all the peers
        {
          for (PermissionMap::iterator iter = mPermissions.begin(); iter != mPermissions.end(); ++iter) {
            PermissionPtr permission = (*iter).second;
            permissionRequest->mPeerAddressList.push_back(permission->mPeerAddress);
          }
        }
        permissionRequest->mUsername = mUsername;
        permissionRequest->mPassword = mPassword;
        permissionRequest->mRealm = mRealm;
        permissionRequest->mNonce = mNonce;
        permissionRequest->mCredentialMechanism = STUNPacket::CredentialMechanisms_LongTerm;
        mPermissionRequester = ISTUNRequester::create(getAssociatedMessageQueue(), mThisWeak.lock(), mActiveServer->mServerIP, permissionRequest, STUNPacket::RFC_5766_TURN);

        // scope: remember which ones will become marked as having permission based on this request completing...
        {
          for (PermissionMap::iterator iter = mPermissions.begin(); iter != mPermissions.end(); ++iter) {
            PermissionPtr permission = (*iter).second;
            if (!permission->mInstalled) {
              permission->mInstallingWithRequester = mPermissionRequester;
            }
          }
        }
      }

      //-----------------------------------------------------------------------
      void TURNSocket::refreshNow()
      {
        if (mRefreshRequester) {
          ZS_LOG_TRACE(log("refresh timer not started as already have an outstanding refresh requester"))
          return;
        }

        if (!mRefreshTimer) {
          ZS_LOG_TRACE(log("cannot perform a refresh as refresh timer for TURN socket is not setup thus not in a state to perform refreshes"))
          return;
        }

        mLastRefreshTimerWasSentAt = zsLib::now();

        ZS_LOG_DEBUG(log("refresh requester starting now"))

        ZS_THROW_INVALID_ASSUMPTION_IF(!mActiveServer)

        // this is the refresh timer... time to perform another refresh now...
        STUNPacketPtr newRequest = STUNPacket::createRequest(STUNPacket::Method_Refresh);
        fix(newRequest);
        newRequest->mUsername = mUsername;
        newRequest->mPassword = mPassword;
        newRequest->mRealm = mRealm;
        newRequest->mNonce = mNonce;
        newRequest->mCredentialMechanism = STUNPacket::CredentialMechanisms_LongTerm;
        mRefreshRequester = ISTUNRequester::create(getAssociatedMessageQueue(), mThisWeak.lock(), mActiveServer->mServerIP, newRequest, STUNPacket::RFC_5766_TURN);
      }
      
      //-----------------------------------------------------------------------
      void TURNSocket::refreshChannels()
      {
        // scope: clean out any channels that haven't seen data in a while
        {
          Time time = zsLib::now();

          typedef std::list<ChannelInfoPtr> InfoList;
          InfoList infoList;

          // scope: find all those which have expired
          {
            for (ChannelNumberMap::iterator iter = mChannelNumberMap.begin(); iter != mChannelNumberMap.end(); ++iter) {
              ChannelInfoPtr info = (*iter).second;
              if (time > (info->mLastSentDataAt + Seconds(OPENPEER_SERVICES_TURN_REMOVE_CHANNEL_IF_NO_DATA_IN_SECONDS))) {
                infoList.push_back(info);
              }
            }
          }

          // clean out expired channels now...
          {
            for (InfoList::iterator iter = infoList.begin(); iter != infoList.end(); ++iter) {
              ChannelInfoPtr info = (*iter);
              ChannelIPMap::iterator found1 = mChannelIPMap.find(info->mPeerAddress);
              ChannelNumberMap::iterator found2 = mChannelNumberMap.find(info->mChannelNumber);

              if (info->mRefreshTimer) {
                info->mRefreshTimer->cancel();
                info->mRefreshTimer.reset();
              }

              if (info->mChannelBindRequester) {
                info->mChannelBindRequester->cancel();
                info->mChannelBindRequester.reset();
              }

              if (found1 != mChannelIPMap.end())
                mChannelIPMap.erase(found1);
              if (found2 != mChannelNumberMap.end())
                mChannelNumberMap.erase(found2);
            }
          }
        }

        // we now have cleaned out expired channels... now we should request channel bindings for those channels which have not expired
        {
          for (ChannelNumberMap::iterator iter = mChannelNumberMap.begin(); iter != mChannelNumberMap.end(); ++iter) {
            ChannelInfoPtr info = (*iter).second;
            if (! (info->mRefreshTimer)) {
              info->mRefreshTimer = Timer::create(mThisWeak.lock(), Seconds(9*10)); // channel bindings last for 10 minutes so refresh at 9 minutes
            }

            if ((!info->mBound) &&
                (!(info->mChannelBindRequester))) {
              // cause a fake timer event to fire immediately to cause the channel to be bound now...
              ITimerDelegatePtr delegate = ITimerDelegateProxy::create(mThisWeak.lock());
              delegate->onTimer(info->mRefreshTimer);
            }
          }
        }
      }

      //-----------------------------------------------------------------------
      bool TURNSocket::sendPacketOrDopPacketIfBufferFull(
                                                         ServerPtr server,
                                                         const BYTE *buffer,
                                                         size_t bufferSizeInBytes
                                                         )
      {
        ITURNSocketDelegatePtr delegate;
        TURNSocketPtr pThis;
        IPAddress serverIP;

        // scope: can't be in the context of a lock when calling the delegate routine
        {
          AutoRecursiveLock lock(mLock);
          if (isShutdown()) {
            OPENPEER_SERVICES_WIRE_LOG_WARNING(Debug, log("send packet failed as TURN is shutdown"))
            return false;
          }

          ZS_THROW_INVALID_ARGUMENT_IF(!server)

          if (!server->mIsUDP) {
            if ((server->mTCPSocket) &&
                (server->mIsConnected)) {
              return sendPacketOverTCPOrDropIfBufferFull(server, buffer, bufferSizeInBytes);
            }
            OPENPEER_SERVICES_WIRE_LOG_WARNING(Detail, log("cannot send packet to server as TCP connection is not connected") + ZS_PARAM("server IP", server->mServerIP.string()))
            return false;
          }

          // this is illegal...

          delegate = mDelegate;
          mLastSentDataToServer = zsLib::now();
          pThis = mThisWeak.lock();
          serverIP = server->mServerIP;
        }

        try {
          return delegate->notifyTURNSocketSendPacket(pThis, serverIP, buffer, bufferSizeInBytes);
        } catch(ITURNSocketDelegateProxy::Exceptions::DelegateGone &) {
          OPENPEER_SERVICES_WIRE_LOG_WARNING(Debug, log("send packet failed as TURN delegate is gone"))
          cancel();
        }
        return false;
      }

      //-----------------------------------------------------------------------
      bool TURNSocket::sendPacketOverTCPOrDropIfBufferFull(
                                                           ServerPtr server,
                                                           const BYTE *buffer,
                                                           size_t bufferSizeInBytes
                                                           )
      {
        ZS_THROW_INVALID_ARGUMENT_IF(!server)

        if (isShutdown()) {
          OPENPEER_SERVICES_WIRE_LOG_WARNING(Debug, log("send packet failed as TURN socket is shutdown") + ZS_PARAM("server IP", server->mServerIP.string()))
          return false;
        }

        if (!server->mTCPSocket) {
          OPENPEER_SERVICES_WIRE_LOG_WARNING(Debug, log("send packet failed as TCP socket is not set") + ZS_PARAM("server IP", server->mServerIP.string()))
          return false;
        }
        if (!server->mIsConnected) {
          OPENPEER_SERVICES_WIRE_LOG_WARNING(Debug, log("send packet failed as TCP socket is not connected") + ZS_PARAM("server IP", server->mServerIP.string()))
          return false;
        }

        // never allow the buffer to overflow
        if (bufferSizeInBytes > sizeof(server->mWriteBuffer)) {
          OPENPEER_SERVICES_WIRE_LOG_WARNING(Debug, log("send packet failed as sending data is over capacity to write buffer") + ZS_PARAM("server IP", server->mServerIP.string()) + ZS_PARAM("sending bytes", bufferSizeInBytes) + ZS_PARAM("capacity", sizeof(server->mWriteBuffer)))
          return false;
        }

        // first try to send the data directly into TCP socket buffer (if possible) - to bypass unrequired copy when the buffer is fully empty
        if (0 == server->mWriteBufferFilledSizeInBytes) {
          if ((NULL != buffer) && (0 != bufferSizeInBytes)) {
            try {
              bool wouldBlock = false;
              mLastSentDataToServer = zsLib::now();
              size_t sent = server->mTCPSocket->send(buffer, bufferSizeInBytes, &wouldBlock);
              if (sent != bufferSizeInBytes) {
                server->mWriteBufferFilledSizeInBytes = (bufferSizeInBytes - sent);

                // we were unable to send the entire data, we should buffer the remainer of the packet to send later...
                memcpy(&(server->mWriteBuffer[0]), &(buffer[sent]), server->mWriteBufferFilledSizeInBytes);
              } else {
                if (server == mActiveServer) {
                  informWriteReady();
                }
              }
            } catch(Socket::Exceptions::Unspecified &error) {
              OPENPEER_SERVICES_WIRE_LOG_WARNING(Debug, log("TCP socket send failure") + ZS_PARAM("error", error.errorCode()))

              cancel();
              return false;
            }
          }

          // nothing more we can do right now...
          return true;
        }

        bool wasRoom = false;

        // if there is room in the end of the buffer for this new packet, tack it on...
        if (bufferSizeInBytes + server->mWriteBufferFilledSizeInBytes < sizeof(server->mWriteBuffer)) {
          if ((NULL != buffer) && (0 != bufferSizeInBytes)) {
            // yup, there's room - tack it on the end...
            memcpy(&(server->mWriteBuffer[server->mWriteBufferFilledSizeInBytes]), buffer, bufferSizeInBytes);
            server->mWriteBufferFilledSizeInBytes += bufferSizeInBytes;
            wasRoom = true;
          }
        }

        if (0 == server->mWriteBufferFilledSizeInBytes) {
          // if nothing in the buffer then nothing to do...
          if (server == mActiveServer) {
            informWriteReady();
          }
          if (!wasRoom) {
            OPENPEER_SERVICES_WIRE_LOG_WARNING(Debug, log("another case of send packet failed as there was not enough buffer space (but yet it seemed to have been sent - strange??)"))
          }
          return wasRoom;
        }

        // we should see if we can send more of the buffer now...
        try {
          bool wouldBlock = false;
          mLastSentDataToServer = zsLib::now();
          size_t sent = server->mTCPSocket->send(&(server->mWriteBuffer[0]), server->mWriteBufferFilledSizeInBytes, &wouldBlock);
          if (sent == server->mWriteBufferFilledSizeInBytes) {
            // we have exhasted the send buffer - horray! nothing more to do now...
            server->mWriteBufferFilledSizeInBytes = 0;
            if (!wasRoom) {
              OPENPEER_SERVICES_WIRE_LOG_WARNING(Debug, log("data was sent over the wire but buffer capacity was reached"))
            }
            return wasRoom;
          }

          if (0 != sent) {
            // we have to consume what was sent from the write buffer
            server->mWriteBufferFilledSizeInBytes -= sent;
            memcpy(&(server->mWriteBuffer[0]), &(server->mWriteBuffer[sent]), server->mWriteBufferFilledSizeInBytes);
            if (0 == server->mWriteBufferFilledSizeInBytes) {
              if (server == mActiveServer) {
                informWriteReady();
              }
            }
          }
        } catch(Socket::Exceptions::Unspecified &error) {
          OPENPEER_SERVICES_WIRE_LOG_WARNING(Debug, log("TCP socket send failure") + ZS_PARAM("error", error.errorCode()))

          cancel();
          return false;
        }
        if (!wasRoom) {
          OPENPEER_SERVICES_WIRE_LOG_WARNING(Debug, log("data was not completely sent over the wire and buffer capacity was reached"))
        }
        return wasRoom;
      }

      //-----------------------------------------------------------------------
      void TURNSocket::informWriteReady()
      {
        if (isShutdown()) return;

        if (!mActiveServer) {
          OPENPEER_SERVICES_WIRE_LOG_WARNING(Trace, log("notify write ready does not have an active server yet (probably okay if during TURN setup process)"))
          return;
        }

        if (mActiveServer->mInformedWriteReady) {
          return;
        }

        try {
          mDelegate->onTURNSocketWriteReady(mThisWeak.lock());
          mActiveServer->mInformedWriteReady = true;
        } catch (ITURNSocketDelegateProxy::Exceptions::DelegateGone &) {
          cancel();
        }
      }

      //-----------------------------------------------------------------------
      WORD TURNSocket::getNextChannelNumber()
      {
        if (mChannelNumberMap.size() > 100) return 0; // already too many allocations - do not do a bind...

        WORD channel = 0;

        CryptoPP::AutoSeededRandomPool rng;
        ULONG attempts = 0;

        do {
          ++attempts;
          if (attempts > 100) return 0; // could not find a free channel in time... abandon effort...

          rng.GenerateBlock((BYTE *)(&channel), sizeof(channel));
          channel = (channel % (mLimitChannelToRangeEnd - mLimitChannelToRangeStart)) + mLimitChannelToRangeStart;

        } while(mChannelNumberMap.find(channel) != mChannelNumberMap.end());

        return channel;
      }

      //-----------------------------------------------------------------------
      ISTUNRequesterPtr TURNSocket::handleAuthorizationErrors(ISTUNRequesterPtr requester, STUNPacketPtr response)
      {
        if (0 == response->mErrorCode) return ISTUNRequesterPtr();
        if (STUNPacket::Class_ErrorResponse != response->mClass) return ISTUNRequesterPtr();

        STUNPacketPtr newRequest;

        // this request failed - why?
        switch (response->mErrorCode) {
          case STUNPacket::ErrorCode_Unauthorized:                  {
            newRequest = (requester->getRequest())->clone(true);
            if (newRequest->mUsername == mUsername) {
              // we tried once but failed to login, give up
              newRequest.reset();
              break;
            }
            mRealm = response->mRealm;
            mNonce = response->mNonce;
            newRequest->mUsername = mUsername;
            newRequest->mPassword = mPassword;
            newRequest->mNonce = mNonce;
            newRequest->mRealm = mRealm;
            newRequest->mCredentialMechanism = STUNPacket::CredentialMechanisms_LongTerm;
            break;
          }
          case STUNPacket::ErrorCode_StaleNonce:                    {
            if ((requester->getRequest())->mTotalRetries > 0) {
              // we already tried once and got the NONCE error before so stop retrying the request
              newRequest.reset();
              break;
            }
            mNonce = response->mNonce;
            if (!response->mRealm.isEmpty())
              mRealm = response->mRealm;

            newRequest = (requester->getRequest())->clone(true);
            newRequest->mTotalRetries = ((requester->getRequest())->mTotalRetries) + 1;
            newRequest->mNonce = mNonce;
            newRequest->mRealm = mRealm;
            newRequest->mCredentialMechanism = STUNPacket::CredentialMechanisms_LongTerm;
            break;
          }
        }

        if (!newRequest) return ISTUNRequesterPtr();
        return ISTUNRequester::create(getAssociatedMessageQueue(), mThisWeak.lock(), requester->getServerIP(), newRequest, STUNPacket::RFC_5766_TURN, requester->getMaxTimeout());
      }

      //-----------------------------------------------------------------------
      void TURNSocket::clearBackgroundingNotifierIfPossible()
      {
        if (!mBackgroundingNotifier) return;
        if (mRefreshRequester) return;
        if (mDeallocateRequester) return;
        if (mPermissionRequester) return;

        ZS_LOG_DEBUG(log("ready to go to the background"))

        mBackgroundingNotifier.reset();
      }
      
      //-------------------------------------------------------------------------
      void TURNSocket::getBuffer(RecycledPacketBuffer &outBuffer)
      {
        AutoRecursiveLock lock(mLock);
        if (mRecycledBuffers.size() < 1) {
          outBuffer = RecycledPacketBuffer(new BYTE[OPENPEER_SERVICES_TURNSOCKET_RECYCLE_BUFFER_SIZE]);
          return;
        }

        outBuffer = mRecycledBuffers.front();
        mRecycledBuffers.pop_front();
      }

      //-------------------------------------------------------------------------
      void TURNSocket::recycleBuffer(RecycledPacketBuffer &buffer)
      {
        AutoRecursiveLock lock(mLock);
        if (!buffer) return;

        if (mRecycledBuffers.size() >= OPENPEER_SERVICES_TURNSOCKET_MAX_RECYLCE_BUFFERS) {
          buffer.reset();
          return;
        }
        mRecycledBuffers.push_back(buffer);
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark TURNSocket::Server
      #pragma mark

      //-----------------------------------------------------------------------
      TURNSocket::Server::Server() :
        mIsUDP(true),
        mIsConnected(false),
        mInformedWriteReady(false),
        mReadBufferFilledSizeInBytes(0),
        mWriteBufferFilledSizeInBytes(0)
      {
        memset(&(mReadBuffer[0]), 0, sizeof(mReadBuffer));
        memset(&(mWriteBuffer[0]), 0, sizeof(mWriteBuffer));
      }

      //-----------------------------------------------------------------------
      TURNSocket::Server::~Server()
      {
        if (mTCPSocket) {
          mTCPSocket->close();
          mTCPSocket.reset();
        }
        if (mAllocateRequester) {
          mAllocateRequester->cancel();
          mAllocateRequester.reset();
        }
      }

      //-----------------------------------------------------------------------
      TURNSocket::ServerPtr TURNSocket::Server::create()
      {
        ServerPtr pThis(new Server);
        return pThis;
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark TURNSocket::CompareIP
      #pragma mark

      //-----------------------------------------------------------------------
      bool TURNSocket::CompareIP::operator()(const IPAddress &op1, const IPAddress &op2) const
      {
        return op1 < op2;
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark TURNSocket::Permission
      #pragma mark

      //-----------------------------------------------------------------------
      TURNSocket::PermissionPtr TURNSocket::Permission::create()
      {
        PermissionPtr pThis(new Permission);
        pThis->mInstalled = false;
        pThis->mLastSentDataAt = zsLib::now();
        return pThis;
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark TURNSocket::ChannelInfo
      #pragma mark

      //-----------------------------------------------------------------------
      TURNSocket::ChannelInfoPtr TURNSocket::ChannelInfo::create()
      {
        ChannelInfoPtr pThis(new ChannelInfo);
        pThis->mBound = 0;
        pThis->mChannelNumber = 0;
        pThis->mLastSentDataAt = zsLib::now();
        return pThis;
      }

    }

    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    #pragma mark
    #pragma mark ITURNSocket
    #pragma mark

    //-------------------------------------------------------------------------
    ITURNSocketPtr ITURNSocket::create(
                                       IMessageQueuePtr queue,
                                       ITURNSocketDelegatePtr delegate,
                                       const char *turnServer,
                                       const char *turnServerUsername,
                                       const char *turnServerPassword,
                                       bool useChannelBinding,
                                       WORD limitChannelToRangeStart,
                                       WORD limitChannelRoRangeEnd
                                       )
    {
      return internal::ITURNSocketFactory::singleton().create(queue, delegate, turnServer, turnServerUsername, turnServerPassword, useChannelBinding, limitChannelToRangeStart, limitChannelRoRangeEnd);
    }

    //-------------------------------------------------------------------------
    ITURNSocketPtr ITURNSocket::create(
                                       IMessageQueuePtr queue,
                                       ITURNSocketDelegatePtr delegate,
                                       IDNS::SRVResultPtr srvTURNUDP,
                                       IDNS::SRVResultPtr srvTURNTCP,
                                       const char *turnServerUsername,
                                       const char *turnServerPassword,
                                       bool useChannelBinding,
                                       WORD limitChannelToRangeStart,
                                       WORD limitChannelRoRangeEnd
                                       )
    {
      return internal::ITURNSocketFactory::singleton().create(queue, delegate, srvTURNUDP, srvTURNTCP, turnServerUsername, turnServerPassword, useChannelBinding, limitChannelToRangeStart, limitChannelRoRangeEnd);
    }

    //-------------------------------------------------------------------------
    ElementPtr ITURNSocket::toDebug(ITURNSocketPtr socket)
    {
      return internal::TURNSocket::toDebug(socket);
    }

  }
}
