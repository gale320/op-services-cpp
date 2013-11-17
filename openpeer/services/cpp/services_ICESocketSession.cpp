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

#ifdef _WIN32
#define NOMINMAX
#endif //WIN32

#include <openpeer/services/internal/services_ICESocketSession.h>
#include <openpeer/services/internal/services_ICESocket.h>
#include <openpeer/services/internal/services_Helper.h>

#include <openpeer/services/IICESocket.h>
#include <openpeer/services/ISTUNRequester.h>

#include <zsLib/Exception.h>
#include <zsLib/helpers.h>
#include <zsLib/Stringize.h>
#include <zsLib/helpers.h>

#include <cryptopp/osrng.h>

#define OPENPEER_SERVICES_ICESOCKETSESSION_MAX_WAIT_TIME_FOR_CANDIDATE_TO_ACTIVATE_IF_ALL_DONE (60)
#define OPENPEER_SERVICES_ICESOCKETSESSION_DEFAULT_KEEPALIVE_INDICATION_TIME_IN_SECONDS (15)

#define OPENPEER_SERVICES_ICESOCKETSESSION_MAX_REASONABLE_CANDIDATE_PAIR_SEARCHES (100)

#define OPENPEER_SERVICES_ICESOCKETSESSION_ACTIVATE_TIMER_IN_MS (20)
#define OPENPEER_SERVICES_ICESOCKETSESSION_STEP_TIMER_IN_SECONDS (2)

namespace openpeer { namespace services { ZS_DECLARE_SUBSYSTEM(openpeer_services_ice) } }

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
      #pragma mark (helpers)
      #pragma mark

      //-----------------------------------------------------------------------
      static QWORD randomQWORD()
      {
        BYTE buffer[sizeof(QWORD)];

        CryptoPP::AutoSeededRandomPool rng;
        rng.GenerateBlock(&(buffer[0]), sizeof(buffer));

        return *((QWORD *)(&(buffer[0])));
      }

      //-----------------------------------------------------------------------
      static QWORD calculatePriority(const IICESocket::Candidate &controlling, const IICESocket::Candidate &controlled) {
        QWORD priorityControlling = controlling.mPriority;
        QWORD priorityControlled = controlled.mPriority;

        QWORD priority = ((((QWORD)1) << 32) * (std::min(priorityControlling, priorityControlled))) +
                          (((QWORD)2) * std::max(priorityControlling, priorityControlled)) +
                          (priorityControlling > priorityControlled ? 1 : 0);
        return priority;
      }

      //-----------------------------------------------------------------------
      static bool comparePairControlling(const ICESocketSession::CandidatePairPtr pair1, const ICESocketSession::CandidatePairPtr pair2) {
        QWORD priorityPair1 = calculatePriority(pair1->mLocal, pair1->mRemote);
        QWORD priorityPair2 = calculatePriority(pair2->mLocal, pair2->mRemote);

        return priorityPair1 > priorityPair2; // pair1 comes before pair2 if pair1 priority is greater
      }

      //-----------------------------------------------------------------------
      static bool comparePairControlled(const ICESocketSession::CandidatePairPtr pair1, const ICESocketSession::CandidatePairPtr pair2) {
        QWORD priorityPair1 = calculatePriority(pair1->mRemote, pair1->mLocal);
        QWORD priorityPair2 = calculatePriority(pair2->mRemote, pair2->mLocal);

        return priorityPair1 > priorityPair2; // pair1 comes before pair2 if pair1 priority is greater
      }

      //-----------------------------------------------------------------------
      static IICESocket::Types normalize(IICESocket::Types transport)
      {
        if (transport == ICESocket::Type_Relayed)
          return ICESocket::Type_Relayed;
        return ICESocket::Type_Local;
      }

      //-----------------------------------------------------------------------
      static IPAddress getViaLocalIP(const ICESocket::Candidate &candidate)
      {
        switch (candidate.mType) {
          case IICESocket::Type_Unknown:          break;
          case IICESocket::Type_Local:            return candidate.mIPAddress;
          case IICESocket::Type_ServerReflexive:
          case IICESocket::Type_PeerReflexive:
          case IICESocket::Type_Relayed:          return candidate.mRelatedIP;
        }
        return (candidate.mRelatedIP.isEmpty() ? candidate.mIPAddress : candidate.mRelatedIP);
      }

      //-----------------------------------------------------------------------
      static bool isCandidateMatch(
                                   const ICESocketSession::CandidatePairPtr &pair,
                                   const IICESocket::Candidate &viaLocalCandidate,
                                   const IPAddress &source
                                   )
      {
        if (!pair) return false;
        if (!pair->mRemote.mIPAddress.isEqualIgnoringIPv4Format(source)) return false;
        if (normalize(pair->mLocal.mType) != normalize(viaLocalCandidate.mType)) return false;
        if (!getViaLocalIP(viaLocalCandidate).isEqualIgnoringIPv4Format(getViaLocalIP(pair->mLocal))) return false;
        if (IICESocket::Type_Relayed == normalize(pair->mLocal.mType)) {
          if (!viaLocalCandidate.mRelatedIP.isEqualIgnoringIPv4Format(pair->mLocal.mRelatedIP)) return false;
        }
        return true;
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark IICESocketSessionForICESocket
      #pragma mark

      //-----------------------------------------------------------------------
      ICESocketSessionPtr IICESocketSessionForICESocket::create(
                                                                IMessageQueuePtr queue,
                                                                IICESocketSessionDelegatePtr delegate,
                                                                ICESocketPtr socket,
                                                                const char *remoteUsernameFrag,
                                                                const char *remotePassword,
                                                                ICEControls control,
                                                                IICESocketSessionPtr foundation
                                                                )
      {
        return IICESocketSessionFactory::singleton().create(queue, delegate, socket, remoteUsernameFrag, remotePassword, control, foundation);
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark ICESocketSession::CandidatePair
      #pragma mark

      //-----------------------------------------------------------------------
      ICESocketSession::CandidatePairPtr ICESocketSession::CandidatePair::create()
      {
        CandidatePairPtr pThis(new CandidatePair);
        pThis->mReceivedRequest = false;
        pThis->mReceivedResponse = false;
        pThis->mFailed = false;
        return pThis;
      }

      //-----------------------------------------------------------------------
      String ICESocketSession::CandidatePair::toDebugString(bool includeCommaPrefix) const
      {
        bool firstTime = false;
        return
        (includeCommaPrefix ? String(", ") : String()) +
        "local candidate: [" + mLocal.toDebugString(false) +
        "], remote candidate: [" + mRemote.toDebugString(false) + "]" +
        Helper::getDebugValue("received request", mReceivedRequest ? String("true") : String(), firstTime) +
        Helper::getDebugValue("received response", mReceivedResponse ? String("true") : String(), firstTime) +
        Helper::getDebugValue("failed", mFailed ? String("true") : String(), firstTime) +
        Helper::getDebugValue("requester", mRequester ? String("true") : String(), firstTime);
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark ICESocketSession
      #pragma mark

      //-----------------------------------------------------------------------
      ICESocketSession::ICESocketSession(
                                         IMessageQueuePtr queue,
                                         IICESocketSessionDelegatePtr delegate,
                                         ICESocketPtr socket,
                                         const char *remoteUsernameFrag,
                                         const char *remotePassword,
                                         ICEControls control,
                                         IICESocketSessionPtr foundation
                                         ) :
        MessageQueueAssociator(queue),
        mICESocketWeak(socket),
        mCurrentState(ICESocketSessionState_Pending),
        mFoundation(ICESocketSession::convert(foundation)),
        mRemoteUsernameFrag(remoteUsernameFrag),
        mRemotePassword(remotePassword),
        mControl(control),
        mConflictResolver(randomQWORD()),
        mLastSentData(zsLib::now()),
        mLastActivity(zsLib::now()),
        mLastReceivedDataOrSTUN(zsLib::now()),
        mKeepAliveDuration(Seconds(OPENPEER_SERVICES_ICESOCKETSESSION_DEFAULT_KEEPALIVE_INDICATION_TIME_IN_SECONDS))
      {
        ZS_LOG_BASIC(log("created"))

        mLocalUsernameFrag = getSocket()->getUsernameFrag();
        mLocalPassword = getSocket()->getPassword();

        if (delegate) {
          mDefaultSubscription = mSubscriptions.subscribe(delegate);
        }
      }

      //-----------------------------------------------------------------------
      void ICESocketSession::init()
      {

        AutoRecursiveLock lock(getLock());
        mSocketSubscription = getSocket()->subscribe(mThisWeak.lock());
        step();
      }

      //-----------------------------------------------------------------------
      ICESocketSession::~ICESocketSession()
      {
        if (isNoop()) return;
        
        mThisWeak.reset();
        ZS_LOG_BASIC(log("destroyed"))
        cancel();
      }

      //-----------------------------------------------------------------------
      ICESocketSessionPtr ICESocketSession::convert(IICESocketSessionPtr session)
      {
        return boost::dynamic_pointer_cast<ICESocketSession>(session);
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark ICESocketSession => IICESocketSession
      #pragma mark

      //-----------------------------------------------------------------------
      String ICESocketSession::toDebugString(IICESocketSessionPtr session, bool includeCommaPrefix)
      {
        if (!session) return String(includeCommaPrefix ? ", ice socket=(null)" : "ice socket=(null)");

        ICESocketSessionPtr pThis = ICESocketSession::convert(session);
        return pThis->getDebugValueString(includeCommaPrefix);
      }
      
      //-----------------------------------------------------------------------
      IICESocketPtr ICESocketSession::getSocket()
      {
        ICESocketPtr socket = mICESocketWeak.lock();
        if (!socket) return IICESocketPtr();
        return socket->forICESocketSession().getSocket();
      }

      //-----------------------------------------------------------------------
      IICESocketSessionSubscriptionPtr ICESocketSession::subscribe(IICESocketSessionDelegatePtr originalDelegate)
      {
        AutoRecursiveLock lock(getLock());
        if (!originalDelegate) return mDefaultSubscription;

        IICESocketSessionSubscriptionPtr subscription = mSubscriptions.subscribe(originalDelegate);

        IICESocketSessionDelegatePtr delegate = mSubscriptions.delegate(subscription);

        if (delegate) {
          ICESocketSessionPtr pThis = mThisWeak.lock();

          if (ICESocketSessionState_Pending != mCurrentState) {
            delegate->onICESocketSessionStateChanged(pThis, mCurrentState);
          }
        }

        if (isShutdown()) {
          mSubscriptions.clear();
        }
        
        return subscription;
      }

      //-----------------------------------------------------------------------
      ICESocketSession::ICESocketSessionStates ICESocketSession::getState(
                                                                          WORD *outLastErrorCode,
                                                                          String *outLastErrorReason
                                                                          ) const
      {
        AutoRecursiveLock lock(getLock());
        if (outLastErrorCode) *outLastErrorCode = mLastError;
        if (outLastErrorReason) *outLastErrorReason = mLastErrorReason;
        return mCurrentState;
      }

      //-----------------------------------------------------------------------
      void ICESocketSession::close()
      {
        ZS_LOG_DEBUG(log("close requested"))
        AutoRecursiveLock lock(getLock());
        cancel();
      }

      //-----------------------------------------------------------------------
      String ICESocketSession::getLocalUsernameFrag() const
      {
        AutoRecursiveLock lock(getLock());
        return mLocalUsernameFrag;
      }

      //-----------------------------------------------------------------------
      String ICESocketSession::getLocalPassword() const
      {
        AutoRecursiveLock lock(getLock());
        return mLocalPassword;
      }

      //-----------------------------------------------------------------------
      String ICESocketSession::getRemoteUsernameFrag() const
      {
        AutoRecursiveLock lock(getLock());
        return mRemoteUsernameFrag;
      }

      //-----------------------------------------------------------------------
      String ICESocketSession::getRemotePassword() const
      {
        AutoRecursiveLock lock(getLock());
        return mRemotePassword;
      }

      //-----------------------------------------------------------------------
      void ICESocketSession::getLocalCandidates(CandidateList &outCandidates)
      {
        outCandidates.clear();

        AutoRecursiveLock lock(getLock());
        IICESocketPtr socket = getSocket();
        if (!socket) return;

        socket->getLocalCandidates(outCandidates);
      }

      //-----------------------------------------------------------------------
      void ICESocketSession::updateRemoteCandidates(const CandidateList &remoteCandidates)
      {
        ZS_LOG_DEBUG(log("updating remote candidates") + ", size=" + string(remoteCandidates.size()))
        AutoRecursiveLock lock(getLock());

        mUpdatedRemoteCandidates = remoteCandidates;
        step();
      }

      //-----------------------------------------------------------------------
      void ICESocketSession::endOfRemoteCandidates()
      {
        ZS_LOG_DEBUG(log("end of remote candidates"))

        AutoRecursiveLock lock(getLock());
        get(mEndOfRemoteCandidatesFlag) = true;
        step();
      }

      //-----------------------------------------------------------------------
      void ICESocketSession::setKeepAliveProperties(
                                                    Duration sendKeepAliveIndications,
                                                    Duration expectSTUNOrDataWithinWithinOrSendAliveCheck,
                                                    Duration keepAliveSTUNRequestTimeout,
                                                    Duration backgroundingTimeout
                                                    )
      {
        AutoRecursiveLock lock(getLock());

        ZS_LOG_DEBUG(log("adjusting keep alive propertiess") +
                     ", send keep alive (ms)=" + string(sendKeepAliveIndications.total_milliseconds()) +
                     ", expecting data within (ms)=" + string(expectSTUNOrDataWithinWithinOrSendAliveCheck.total_milliseconds()))

        if (mKeepAliveTimer) {
          ZS_LOG_DEBUG(log("cancelling current keep alive timer"))
          mKeepAliveTimer->cancel();
          mKeepAliveTimer.reset();
        }

        if (mAliveCheckRequester) {
          ZS_LOG_DEBUG(log("cancelling current alive check requester"))
          mAliveCheckRequester->cancel();
          mAliveCheckRequester.reset();
        }

        if (mExpectingDataTimer) {
          ZS_LOG_DEBUG(log("cancelling current expecting data timer"))
          mExpectingDataTimer->cancel();
          mExpectingDataTimer.reset();
        }

        mKeepAliveDuration = sendKeepAliveIndications;
        mExpectSTUNOrDataWithinDuration = expectSTUNOrDataWithinWithinOrSendAliveCheck;
        mKeepAliveSTUNRequestTimeout = keepAliveSTUNRequestTimeout;
        mBackgroundingTimeout = backgroundingTimeout;

        ZS_LOG_DEBUG(log("forcing step to ensure all timers are properly created"))
        (IWakeDelegateProxy::create(mThisWeak.lock()))->onWake();
      }

      //-----------------------------------------------------------------------
      bool ICESocketSession::sendPacket(
                                        const BYTE *packet,
                                        size_t packetLengthInBytes
                                        )
      {
        AutoRecursiveLock lock(getLock());
        if (isShutdown()) {
          ZS_LOG_WARNING(Detail, log("unable to send packet as socket is already shutdown"))
          return false;
        }

        get(mInformedWriteReady) = false;  // if this method was called in response to a write-ready event, be sure to clear the write-ready informed flag so future events will fire

        if (!mNominated) {
          ZS_LOG_WARNING(Detail, log("not allowed to send data as ICE nomination process is not complete"))
          return false;  // do not allow sending when no candidate has been nominated
        }

        mLastSentData = zsLib::now();
        return sendTo(mNominated->mLocal, mNominated->mRemote.mIPAddress, packet, packetLengthInBytes, true);
      }

      //-----------------------------------------------------------------------
      ICESocketSession::ICEControls ICESocketSession::getConnectedControlState()
      {
        AutoRecursiveLock lock(getLock());
        return mControl;
      }

      //-----------------------------------------------------------------------
      IPAddress ICESocketSession::getConnectedRemoteIP()
      {
        AutoRecursiveLock lock(getLock());
        if (!mNominated) return IPAddress();
        return mNominated->mRemote.mIPAddress;
      }

      //-----------------------------------------------------------------------
      bool ICESocketSession::getNominatedCandidateInformation(
                                                              Candidate &outLocal,
                                                              Candidate &outRemote
                                                              )
      {
        AutoRecursiveLock lock(getLock());
        if (isShutdown()) return false;

        CandidatePairPtr resultPair = mNominated ? mNominated : mPreviouslyNominated;

        if (!resultPair) return false;

        outLocal = resultPair->mLocal;
        outRemote = resultPair->mRemote;
        return true;
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark ICESocketSession => IICESocketSessionForICESocket
      #pragma mark

      //-----------------------------------------------------------------------
      ICESocketSessionPtr ICESocketSession::create(
                                                   IMessageQueuePtr queue,
                                                   IICESocketSessionDelegatePtr delegate,
                                                   ICESocketPtr socket,
                                                   const char *remoteUsernameFrag,
                                                   const char *remotePassword,
                                                   ICEControls control,
                                                   IICESocketSessionPtr foundation
                                                   )
      {
        ICESocketSessionPtr pThis(new ICESocketSession(queue, delegate, socket, remoteUsernameFrag, remotePassword, control, foundation));
        pThis->mThisWeak = pThis;
        pThis->init();
        return pThis;
      }

      //-----------------------------------------------------------------------
      bool ICESocketSession::handleSTUNPacket(
                                              const IICESocket::Candidate &viaLocalCandidate,
                                              const IPAddress &source,
                                              STUNPacketPtr stun,
                                              const String &localUsernameFrag,
                                              const String &remoteUsernameFrag
                                              )
      {
        ZS_THROW_INVALID_ARGUMENT_IF(!stun)

        ZS_LOG_DEBUG(log("handle stun packet") + ", candidate: " + viaLocalCandidate.toDebugString(false) + ", source=" + string(source) + ", local username frag=" + localUsernameFrag + ", remote username frag=" + remoteUsernameFrag)

        if (mSubscriptions.size() < 1) {
          ZS_LOG_WARNING(Debug, log("unable to handle STUN packet as no subscribers"))
          return false;
        }

        // inform that the session is now connected
        if (STUNPacket::Method_Binding != stun->mMethod) {
          ZS_LOG_DETAIL(log("received incoming STUN which is not ICE related thus handing via delgate"))
          return mSubscriptions.delegate()->handleICESocketSessionReceivedSTUNPacket(mThisWeak.lock(), stun, localUsernameFrag, remoteUsernameFrag);
        }

        AutoRecursiveLock lock(getLock());

        if (localUsernameFrag != mLocalUsernameFrag) {
          ZS_LOG_DEBUG(log("local username frag does not match") + ", expecting=" + mLocalUsernameFrag + ", received=" + localUsernameFrag)
          return false;
        }

        if (remoteUsernameFrag != mRemoteUsernameFrag) {
          ZS_LOG_DEBUG(log("remote username frag does not match") + ", expecting=" + mRemoteUsernameFrag + ", received=" + remoteUsernameFrag)
          return false;
        }


        CandidatePairPtr found;

        bool failedIntegrity = (!stun->isValidMessageIntegrity(mLocalPassword));
        if (failedIntegrity) goto send_response;

        if (isCandidateMatch(mNominated, viaLocalCandidate, source)) {
          found = mNominated;
        }

        for (CandidatePairList::iterator iter = mCandidatePairs.begin(); (!found) && (iter != mCandidatePairs.end()); ++iter)
        {
          CandidatePairPtr &pair = (*iter);
          if (isCandidateMatch(pair, viaLocalCandidate, source)) {
            found = pair;
            break;
          }
        }

        if (found) {
          ZS_LOG_DEBUG(log("found pairing") + ", is nominated=" + (mNominated == found ? "true":"false")  + found->toDebugString())
        }

        if (!found) {

          CandidateList::iterator foundLocalCandidate = mLocalCandidates.end();

          for (CandidateList::iterator iter = mLocalCandidates.begin(); iter != mLocalCandidates.end(); ++iter)
          {
            Candidate &candidate = (*iter);
            if (candidate.mType != viaLocalCandidate.mType) continue;
            if (getViaLocalIP(candidate) != getViaLocalIP(viaLocalCandidate)) continue;
            if (IICESocket::Type_Relayed == candidate.mType) {
              if (candidate.mRelatedIP != viaLocalCandidate.mRelatedIP) continue;
            }

            foundLocalCandidate = iter;
            break;
          }

          Candidate remote;
          remote.mIPAddress = source;
          remote.mType = IICESocket::Type_PeerReflexive;
          remote.mPriority = ((1 << 24)*(remote.mType)) + ((1 << 8)*(remote.mLocalPreference)) + (256 - 0);

          if (foundLocalCandidate != mLocalCandidates.end()) {
            CandidatePairPtr newPair = CandidatePair::create();
            newPair->mLocal = (*foundLocalCandidate);
            newPair->mRemote = remote;
            newPair->mReceivedRequest = true;

            ZS_LOG_DEBUG(log("new candidate pair discovered") + newPair->toDebugString())

            mCandidatePairs.push_back(newPair);

            found = newPair;
          }

          if (mUpdatedRemoteCandidates.size() < 1) {
            mUpdatedRemoteCandidates = mRemoteCandidates;
          }
          mUpdatedRemoteCandidates.push_back(remote);

          ZS_LOG_DEBUG(log("performing discovery on peer reflexive discovered IP") + ", remote: " + remote.toDebugString(false))

          (IWakeDelegateProxy::create(mThisWeak.lock()))->onWake();

          goto send_response;
        }

        // scope: found an existing canddiate
        {
          found->mReceivedRequest = true;
          found->mFailed = false;                   // even if this previously failed, we are now going to try this request to see if it works

          if (found->mRequester) {
            found->mRequester->retryRequestNow();   // retry the request immediately
          }
        }

      send_response:

        //.....................................................................
        // scope: send response
        {
          bool correctRole = true;
          bool wonConflict = false;

          if (STUNPacket::Class_Indication != stun->mClass) {
            if (!failedIntegrity) {
              // check to see if the request is in the correct role...
              if ((ICESocket::ICEControl_Controlling == mControl) &&
                  (stun->mIceControllingIncluded)) {
                correctRole = false;
                wonConflict = (mConflictResolver >= stun->mIceControlling);
              }

              if ((ICESocket::ICEControl_Controlled == mControl) &&
                  (stun->mIceControlledIncluded)) {
                correctRole = false;
                wonConflict = (mConflictResolver < stun->mIceControlled);
              }

              if (!correctRole) { // one of us is in the incorret role?
                if (!wonConflict) {
                  // we have to switch roles!
                  ZS_LOG_WARNING(Detail, log("candidate role conflict detected thus switching roles"))
                  switchRole(ICESocket::ICEControl_Controlled == mControl ? IICESocket::ICEControl_Controlling : IICESocket::ICEControl_Controlled);
                  return true;
                }

                // we one the conflict but the other party needs to get an error message
              }
            }

            STUNPacketPtr response;

            if ((correctRole) && (!failedIntegrity)) {
              // we need to generate a proper response
              response = STUNPacket::createResponse(stun);
              fix(response);
              response->mMappedAddress = source;
            } else {
              // we need to generate an error response
              if (!correctRole) {
                stun->mErrorCode = STUNPacket::ErrorCode_RoleConflict;
                ZS_LOG_WARNING(Detail, log("candidate role conflict detected thus telling other party to switch roles via an error"))
              }
              if (failedIntegrity) {
                stun->mErrorCode = STUNPacket::ErrorCode_Unauthorized;
                ZS_LOG_ERROR(Detail, log("candidate password integrity failed"))
              }
              response = STUNPacket::createErrorResponse(stun);
              fix(response);
            }

            response->mPassword = mLocalPassword;
            response->mCredentialMechanism = STUNPacket::CredentialMechanisms_ShortTerm;

            boost::shared_array<BYTE> buffer;
            size_t bufferLengthInBytes = 0;
            response->packetize(buffer, bufferLengthInBytes, STUNPacket::RFC_5245_ICE);
            sendTo(viaLocalCandidate, source, buffer.get(), bufferLengthInBytes, false);
          }

          if ((failedIntegrity) || (!correctRole)) {
            ZS_LOG_WARNING(Trace, log("do not handle packet any further when integrity fails or when in incorrect role"))
            return true;
          }
        }

        //.....................................................................
        // scope: handle nomination
        {
          if (found) {
            if ((stun->mUseCandidateIncluded) &&
                (ICESocket::ICEControl_Controlled == mControl)) {

              if (mNominated != found) {
                // the remote party is telling this party that this pair is nominated
                ZS_LOG_DETAIL(log("candidate is nominated by controlling party (i.e. remote party)") + found->toDebugString())

                mNominated = found;

                // this should be happening, but just in case, clear out any nomination process in progress
                mPendingNominatation.reset();
                if (mNominateRequester) {
                  mNominateRequester->cancel();
                  mNominateRequester.reset();
                }

                ICESocketPtr socket = mICESocketWeak.lock();
                if (socket) {
                  socket->forICESocketSession().addRoute(mThisWeak.lock(), mNominated->mRemote.mIPAddress);
                }

                get(mInformedWriteReady) = false;

                notifyLocalWriteReady(viaLocalCandidate);
                notifyRelayWriteReady(viaLocalCandidate);

                (IWakeDelegateProxy::create(mThisWeak.lock()))->onWake();
              }
            }
          }
        }

        //.....................................................................
        // scope: create new requester
        {
          if (found) {
            if (!found->mRequester) {
              if (!found->mReceivedResponse) {
                ZS_LOG_DETAIL(log("candidate search started on reaction to a request") + found->toDebugString())

                STUNPacketPtr request = STUNPacket::createRequest(STUNPacket::Method_Binding);
                fix(request);
                request->mUsername = mRemoteUsernameFrag + ":" + mLocalUsernameFrag;
                if (mRemotePassword.hasData()) {
                  request->mPassword = mRemotePassword;
                }
                request->mPriorityIncluded = true;
                request->mPriority = found->mLocal.mPriority;
                if (mRemotePassword.hasData()) {
                  request->mCredentialMechanism = STUNPacket::CredentialMechanisms_ShortTerm;
                }
                if (IICESocket::ICEControl_Controlling == mControl) {
                  request->mIceControllingIncluded = true;
                  request->mIceControlling = mConflictResolver;
                } else {
                  request->mIceControlledIncluded = true;
                  request->mIceControlled = mConflictResolver;
                }

                // activate the pair search now...
                found->mRequester = ISTUNRequester::create(getAssociatedMessageQueue(), mThisWeak.lock(), found->mRemote.mIPAddress, request, STUNPacket::RFC_5245_ICE);
              }
            }
          }
        }

        //.....................................................................
        // scope: check to see if should consider this data activity
        {
          if (found) {
            if (found == mNominated) {
              mLastReceivedDataOrSTUN = zsLib::now();

              if (mAliveCheckRequester) {
                ZS_LOG_DEBUG(log("alive check requester is no longer needed as STUN request/integrity bind was received"))
                mAliveCheckRequester->cancel();
                mAliveCheckRequester.reset();
              }
            }
          }
        }

        return true;
      }

      //-----------------------------------------------------------------------
      bool ICESocketSession::handlePacket(
                                          const IICESocket::Candidate &viaLocalCandidate,
                                          const IPAddress &source,
                                          const BYTE *packet,
                                          size_t packetLengthInBytes
                                          )
      {
        // WARNING: This method calls a delegate synchronously thus must
        //          never be called from a method that is within a lock.
        {
          AutoRecursiveLock lock(getLock());
          if ((NULL == packet) ||
              (0 == packetLengthInBytes)) {
            ZS_LOG_WARNING(Trace, log("incoming data packet is NULL or of 0 length thus ignoring"))
            return false;
          }

          if (isShutdown()) {
            ZS_LOG_WARNING(Trace, log("already shutdown thus ignoring incoming data packet"))
            return false;
          }

          if (!mNominated) {
            ZS_LOG_WARNING(Trace, log("cannot process data packets without a nominated ice pair"))
            return false;                                          // can't receive if not connected
          }

          if (!isCandidateMatch(mNominated, viaLocalCandidate, source)) {
            ZS_LOG_WARNING(Trace, log("incoming remote IP on data packet does not match nominated canddiate thus ignoring") + ", candidate: " + viaLocalCandidate.toDebugString(false) + ", source=" + string(source) + ", local: " + mNominated->mLocal.toDebugString(false) + ", remote: " + mNominated->mRemote.toDebugString(false))
            return false;
          }

          mLastReceivedDataOrSTUN = zsLib::now();

          if (mAliveCheckRequester) {
            ZS_LOG_DEBUG(log("alive check requester is no longer needed as data was received"))
            mAliveCheckRequester->cancel();
            mAliveCheckRequester.reset();
          }
        }

        // we have a match on the packet... send the data to the delegate...
        mSubscriptions.delegate()->handleICESocketSessionReceivedPacket(mThisWeak.lock(), packet, packetLengthInBytes);
        return true;
      }

      //-----------------------------------------------------------------------
      void ICESocketSession::notifyLocalWriteReady(const IICESocket::Candidate &viaLocalCandidate)
      {
        AutoRecursiveLock lock(getLock());
        if (isShutdown()) return;
        if (mInformedWriteReady) return;

        if (!mNominated) {
          ZS_LOG_TRACE(log("notify local write ready cannot inform delegate since nomination process is incomplete"))
          return;
        }

        if (!isCandidateMatch(mNominated, viaLocalCandidate, mNominated->mRemote.mIPAddress)) {
          ZS_LOG_WARNING(Trace, log("write ready notification does not match") + ", candidate: " + viaLocalCandidate.toDebugString(false))
          return;
        }

        get(mInformedWriteReady) = false;

        ZS_LOG_TRACE(log("notify local write ready"))

        mSubscriptions.delegate()->onICESocketSessionWriteReady(mThisWeak.lock());
        get(mInformedWriteReady) = true;
      }

      //-----------------------------------------------------------------------
      void ICESocketSession::notifyRelayWriteReady(const IICESocket::Candidate &viaLocalCandidate)
      {
        AutoRecursiveLock lock(getLock());
        if (isShutdown()) return;
        if (mInformedWriteReady) return;

        if (!mNominated) {
          ZS_LOG_TRACE(log("notify relay write ready cannot inform delegate since nomination process is incomplete"))
          return;
        }

        if (!isCandidateMatch(mNominated, viaLocalCandidate, mNominated->mRemote.mIPAddress)) {
          ZS_LOG_WARNING(Trace, log("write ready notification does not match") + ", candidate: " + viaLocalCandidate.toDebugString(false))
          return;
        }

        get(mInformedWriteReady) = false;

        ZS_LOG_TRACE(log("notify relay write ready"))

        mSubscriptions.delegate()->onICESocketSessionWriteReady(mThisWeak.lock());
        get(mInformedWriteReady) = true;
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark ICESocketSession => IWakeDelegate
      #pragma mark

      //-----------------------------------------------------------------------
      void ICESocketSession::onWake()
      {
        AutoRecursiveLock lock(getLock());
        ZS_LOG_DEBUG(log("on wake"))
        step();
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark ICESocketSession => IICESocketDelegate
      #pragma mark

      //-----------------------------------------------------------------------
      void ICESocketSession::onICESocketStateChanged(
                                                     IICESocketPtr socket,
                                                     ICESocketStates state
                                                     )
      {
        AutoRecursiveLock lock(getLock());
        ZS_LOG_DEBUG(log("on ice socket state changed"))
        step();
      }

      //-----------------------------------------------------------------------
      void ICESocketSession::onICESocketCandidatesChanged(IICESocketPtr socket)
      {
        AutoRecursiveLock lock(getLock());

        ZS_LOG_DEBUG(log("on ice socket candidates changed"))

        if (isShutdown()) {
          ZS_LOG_WARNING(Detail, log("already shutdown"))
          return;
        }

        socket->getLocalCandidates(mUpdatedLocalCandidates);

        step();
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark ICESocketSession => ISTUNRequesterDelegate
      #pragma mark

      //-----------------------------------------------------------------------
      void ICESocketSession::onSTUNRequesterSendPacket(
                                                       ISTUNRequesterPtr requester,
                                                       IPAddress destination,
                                                       boost::shared_array<BYTE> packet,
                                                       size_t packetLengthInBytes
                                                       )
      {
        ZS_LOG_TRACE(log("on stun requester send packet"))

        AutoRecursiveLock lock(getLock());
        if (isShutdown()) return;

        if (requester == mNominateRequester) {
          ZS_THROW_BAD_STATE_IF(!mPendingNominatation)
          sendTo(mPendingNominatation->mLocal, destination, packet.get(), packetLengthInBytes, false);
          return;
        }

        if (requester == mAliveCheckRequester) {
            ZS_THROW_BAD_STATE_IF(!mNominated)
            sendTo(mNominated->mLocal, destination, packet.get(), packetLengthInBytes, false);
            return;
        }

        // scope: search the candidates to see which one is sending the request
        {
          for (CandidatePairList::iterator iter = mCandidatePairs.begin(); iter != mCandidatePairs.end(); ++iter)
          {
            CandidatePairPtr pairing = (*iter);
            if (requester == pairing->mRequester) {
              sendTo(pairing->mLocal, destination, packet.get(), packetLengthInBytes, false);
              return;
            }
          }
        }

        // must be from an old requester to get here...
      }

      //-----------------------------------------------------------------------
      bool ICESocketSession::handleSTUNRequesterResponse(
                                                         ISTUNRequesterPtr requester,
                                                         IPAddress fromIPAddress,
                                                         STUNPacketPtr response
                                                         )
      {
        ZS_LOG_TRACE(log("handle STUN requester response"))

        AutoRecursiveLock lock(getLock());
        if (isShutdown()) return false;

        if ((requester == mNominateRequester) ||
            (requester == mAliveCheckRequester)) {

          CandidatePairPtr usePair = (requester == mNominateRequester ? mPendingNominatation : mNominated);
          ZS_THROW_BAD_STATE_IF(!usePair)

          if ((0 != response->mErrorCode) ||
              (response->mClass != STUNPacket::Class_Response)) {
            // some kind of error happened during the nominate
            switch (response->mErrorCode) {
              case STUNPacket::ErrorCode_RoleConflict: {
                // this request better be signed properly or we will ignore the conflict...
                if (!mRemotePassword.isEmpty()) {
                  if (!response->isValidMessageIntegrity(mRemotePassword)) {
                    ZS_LOG_WARNING(Detail, log("nomination caused role conflict reply did not pass integtiry check") + usePair->toDebugString())
                    return false;
                  }
                }

                if (requester == mAliveCheckRequester) {
                  ZS_LOG_WARNING(Detail, log("alive check caused role conflict reply cannot be issued for alive check request (since already nominated)") + usePair->toDebugString())
                  return false;
                }

                ZS_LOG_WARNING(Detail, log("nomination request caused role conflict") + usePair->toDebugString())

                // we have a role conflict... switch roles now...
                STUNPacketPtr originalRequest = requester->getRequest();
                switchRole(originalRequest->mIceControlledIncluded ? IICESocket::ICEControl_Controlling : IICESocket::ICEControl_Controlled);
                return true;
              }
              default: break; // well.. that sucks... why would it be errored???
            }

            // handle this in the same way a timeout would be handled (what else can we do?)
            onSTUNRequesterTimedOut(requester);
            return true;
          }

          // the nomination request succeeded (or so we think - make sure it was signed properly)!
          if (mRemotePassword.hasData()) {
            if (!response->isValidMessageIntegrity(mRemotePassword)) {
              ZS_LOG_WARNING(Detail, log("response from nomination or alive check failed message integrity") + ", was nominate requester=" + (requester == mNominateRequester  ? "true" : "false"))
              return false;
            }
          }

          if (requester == mAliveCheckRequester) {
            ZS_LOG_DEBUG(log("alive check request succeeded") + usePair->toDebugString())
            mLastReceivedDataOrSTUN = zsLib::now();

            mAliveCheckRequester.reset();
            return true;
          }

          // yes, okay, it did in fact succeed - we have nominated our candidate!
          // inform that the session is now connected
          ZS_LOG_DETAIL(log("nomination request succeeded") + usePair->toDebugString())

          // we are now established to the remote party

          mNominateRequester.reset();
          mNominated = usePair;
          mPendingNominatation.reset();

          get(mInformedWriteReady) = false;

          notifyLocalWriteReady(usePair->mLocal);
          notifyRelayWriteReady(usePair->mLocal);

          (IWakeDelegateProxy::create(mThisWeak.lock()))->onWake();
          return true;
        }

        // this could be for one of the candidates...
        for (CandidatePairList::iterator iter = mCandidatePairs.begin(); iter != mCandidatePairs.end(); ++iter)
        {
          CandidatePairPtr pairing = (*iter);
          if (requester != pairing->mRequester)
            continue;

          // see what kind of issue we have on the packet
          if ((0 != response->mErrorCode) ||
              (response->mClass != STUNPacket::Class_Response)) {
            // some kind of error happened during the nominate
            switch (response->mErrorCode) {
              case STUNPacket::ErrorCode_RoleConflict: {
                // this request better be signed properly or we will ignore the conflict...
                if (mRemotePassword.hasData()) {
                  if (!response->isValidMessageIntegrity(mRemotePassword)) return false;
                }

                ZS_LOG_WARNING(Detail, log("candidate role conflict error received") + pairing->toDebugString())

                // we have a role conflict... switch roles now...
                STUNPacketPtr originalRequest = requester->getRequest();
                switchRole(originalRequest->mIceControlledIncluded ? IICESocket::ICEControl_Controlling : IICESocket::ICEControl_Controlled);
                return true;
              }
              default: break;
            }
            // we will ignore all other issues
            return true;
          }

          pairing->mFailed = false;
          pairing->mReceivedResponse = true;
          pairing->mRequester.reset();
          if (mRemotePassword.isEmpty()) {
            // fake that we received a request since we will never receive in this case
            pairing->mReceivedRequest = true;
          }
          ZS_LOG_DEBUG(log("pairing response received") + pairing->toDebugString())
          step();
          return true;
        }

        return false;
      }

      //-----------------------------------------------------------------------
      void ICESocketSession::onSTUNRequesterTimedOut(ISTUNRequesterPtr requester)
      {
        ZS_LOG_TRACE(log("on STUN requester timed out"))

        AutoRecursiveLock lock(getLock());

        if (requester == mAliveCheckRequester) {
          ZS_LOG_WARNING(Detail, log("alive connectivity check failed (probably a connection timeout)") + mNominated->toDebugString())

          mAliveCheckRequester.reset();

          mPreviouslyNominated = mNominated;
          mNominated.reset();

          // nomination failed, force scanning to happen now
          mNominated->mReceivedRequest = false;
          mNominated->mReceivedResponse = false;

          if (mNominated->mRequester) {
            mNominated->mRequester->cancel();
            mNominated->mRequester.reset();
          }

          (IWakeDelegateProxy::create(mThisWeak.lock()))->onWake();
          return;
        }

        if (requester == mNominateRequester) {
          mNominateRequester.reset();

          // we were nominating this candidate but it isn't responding! We will not nominate this pair but instead will start the scan again...
          for (CandidatePairList::iterator iter = mCandidatePairs.begin(); iter != mCandidatePairs.end(); ++iter)
          {
            CandidatePairPtr pairing = (*iter);
            if (mPendingNominatation == pairing) {
              ZS_LOG_ERROR(Detail, log("nomination of candidate failed") + pairing->toDebugString())

              // we found the candidate that was going to be nomiated but it can't be since the nomination failed...
              pairing->mFailed = false;
              pairing->mReceivedResponse = false; // mark that we haven't received the reply yet so the scan has to restart
              if (pairing->mRequester) {
                pairing->mRequester->cancel();
                pairing->mRequester.reset();
              }
              break;
            }
          }

          // forget which was nominated since it failed...
          mPendingNominatation.reset();

          // try something else instead...
          (IWakeDelegateProxy::create(mThisWeak.lock()))->onWake();
          return;
        }

        // scope: try to figure out which candidate this requested belonged to...
        {
          for (CandidatePairList::iterator iter = mCandidatePairs.begin(); iter != mCandidatePairs.end(); ++iter)
          {
            CandidatePairPtr pairing = (*iter);
            if (requester == pairing->mRequester) {
              // mark this pair as failed
              ZS_LOG_DETAIL(log("candidate timeout") + pairing->toDebugString())

              pairing->mRequester.reset();
              pairing->mFailed = true;

              step();
              return;
            }
          }
        }
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark ICESocketSession => ITimerDelegate
      #pragma mark

      //-----------------------------------------------------------------------
      void ICESocketSession::onTimer(TimerPtr timer)
      {
        AutoRecursiveLock lock(getLock());
        if (isShutdown()) return;

        Time tick = zsLib::now();
        if (Duration() != mBackgroundingTimeout) {
          Duration diff = tick - mLastActivity;
          if (diff > mBackgroundingTimeout) {
            ZS_LOG_WARNING(Detail, log("backgrounding timeout forced this session to close") + ", time diff (ms)=" + string(diff.total_milliseconds()))
            setError(ICESocketSessionShutdownReason_BackgroundingTimeout, "backgrounding timeout");
            cancel();
            return;
          }
          mLastActivity = tick;
        }

        if (timer == mStepTimer) {
          ZS_LOG_TRACE(log("step timer"))
          step();
          return;
        }

        if (timer == mActivateTimer)
        {
          if (mCandidatePairs.size() < 1) {
            ZS_LOG_TRACE(log("no candidates pairs to activate"))
            return;
          }

          // we are going to activate the next candidate pair now...
          for (CandidatePairList::iterator iter = mCandidatePairs.begin(); iter != mCandidatePairs.end(); ++iter)
          {
            CandidatePairPtr pairing = (*iter);
            if (pairing == mNominated) {
              ZS_LOG_DEBUG(log("cannot activate beyond the point of nomination"))
              break;
            }
            if (pairing->mRequester) continue;
            if (pairing->mReceivedResponse) continue; // no need to activate a second time if a response has been received
            if (pairing->mFailed) continue; // do not activate a pair that has already failed

            if (mFoundation) {
              if (!mFoundation->canUnfreeze(pairing)) {
                if (pairing->mFailed) {
                  ZS_LOG_TRACE(log("candidate now marked as failed (as foundation candidate pairing failed)") + pairing->toDebugString())
                  // need to perform step after failure
                  IWakeDelegateProxy::create(mThisWeak.lock())->onWake();
                  break;
                }
                ZS_LOG_TRACE(log("candidate still frozen") + pairing->toDebugString())
                continue;
              }
            }

            ZS_LOG_DETAIL(log("activating search on candidate") + pairing->toDebugString())

            // activate this pair right now... if there is no remote username then treat this as a regular STUN request/response situation (plus will automatically nominate if successful)
            STUNPacketPtr request = STUNPacket::createRequest(STUNPacket::Method_Binding);
            fix(request);
            bool isICE = false;

            if (mRemotePassword.hasData()) {
              isICE = true;
              request->mUsername = mRemoteUsernameFrag + ":" + mLocalUsernameFrag;
              request->mPassword = mRemotePassword;
              request->mCredentialMechanism = STUNPacket::CredentialMechanisms_ShortTerm;
              request->mPriorityIncluded = true;
              request->mPriority = pairing->mLocal.mPriority;
              if (IICESocket::ICEControl_Controlling == mControl) {
                request->mIceControllingIncluded = true;
                request->mIceControlling = mConflictResolver;
              } else {
                request->mIceControlledIncluded = true;
                request->mIceControlled = mConflictResolver;
              }
            }

            // activate the pair search now...
            pairing->mRequester = ISTUNRequester::create(getAssociatedMessageQueue(), mThisWeak.lock(), pairing->mRemote.mIPAddress, request, (isICE ? STUNPacket::RFC_5245_ICE : STUNPacket::RFC_5389_STUN));
            break;  // only activate one pair at this time
          }
          return;
        }

        if (timer == mKeepAliveTimer)
        {
          if (mNominateRequester) return;  // can't do keep alives during a nomination process
          if (!mNominated) return;  // can't do keep alives if not connected

          // we are going to check the ICE socket to see if it can shutdown TURN at this time...
          if (mLastSentData + mKeepAliveDuration > tick) {
            ZS_LOG_TRACE(log("no need to fire keep alive timer as data was sent within keep alive window"))
            return;  // not enough time has passed since sending data to send more...
          }

          ZS_LOG_DETAIL(log("keep alive") + mNominated->toDebugString())
          STUNPacketPtr indication = STUNPacket::createIndication(STUNPacket::Method_Binding);
          fix(indication);

          if (mRemotePassword.hasData()) {
            indication->mUsername = mRemoteUsernameFrag + ":" + mLocalUsernameFrag;
            indication->mPassword = mRemotePassword;
            indication->mCredentialMechanism = STUNPacket::CredentialMechanisms_ShortTerm;
          }

          boost::shared_array<BYTE> buffer;
          size_t length = 0;
          indication->packetize(buffer, length, STUNPacket::RFC_5245_ICE);
          sendTo(mNominated->mLocal, mNominated->mRemote.mIPAddress, buffer.get(), length, true);
        }

        if (timer == mExpectingDataTimer)
        {
          if (mNominateRequester) return;  // can't do keep alives during a nomination process
          if (!mNominated) return;  // can't do keep alives if not connected

          if (mLastReceivedDataOrSTUN + mExpectSTUNOrDataWithinDuration > tick) {
            ZS_LOG_TRACE(log("received STUN request or indication or data within the expected window so no need to test if remote party is alive"))
            return;
          }

          if (mAliveCheckRequester) {
            ZS_LOG_WARNING(Detail, log("alive check requester already activated"))
            return;
          }

          ZS_LOG_TRACE(log("expecting data timer fired"))

          //...................................................................
          // NOTE: Servers will *NOT* send regular connectivity checks to
          // their clients. This responsibility is leftto the client so
          // the client cannot expect to receive data within a time frame
          // when connecting to a server.
          //
          // However the keep alive check mechanism can be used to probe if
          // a server is still alive as the server will respond to the request
          // albeit without security credentials.

          STUNPacketPtr request = STUNPacket::createRequest(STUNPacket::Method_Binding);
          fix(request);
          bool isICE = false;

          if (mRemotePassword.hasData()) {
            ZS_LOG_WARNING(Detail, log("expected STUN request or indication or data within the expected window but did not receive (thus will attempt to do a connectivity check)"))
            isICE = true;
            request->mUsername = mRemoteUsernameFrag + ":" + mLocalUsernameFrag;
            request->mPassword = mRemotePassword;
            request->mCredentialMechanism = STUNPacket::CredentialMechanisms_ShortTerm;
            request->mIceControllingIncluded = true;
            request->mIceControlling = mConflictResolver;
            request->mPriorityIncluded = true;
            request->mPriority = mNominated->mLocal.mPriority;
          }

          mAliveCheckRequester = ISTUNRequester::create(getAssociatedMessageQueue(), mThisWeak.lock(), mNominated->mRemote.mIPAddress, request, (isICE ? STUNPacket::RFC_5245_ICE : STUNPacket::RFC_5389_STUN), mKeepAliveSTUNRequestTimeout);
          return;
        }
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark ICESocketSession => (internal)
      #pragma mark

      //-----------------------------------------------------------------------
      RecursiveLock &ICESocketSession::getLock() const
      {
        ICESocketPtr socket = mICESocketWeak.lock();
        if (!socket)
          return mBogusLock;
        return socket->forICESocketSession().getLock();
      }

      //-----------------------------------------------------------------------
      String ICESocketSession::log(const char *message) const
      {
        return String("ICESocketSession [") + string(mID) + "] " + message;
      }

      //-----------------------------------------------------------------------
      void ICESocketSession::fix(STUNPacketPtr stun) const
      {
        stun->mLogObject = "ICESocketSession";
        stun->mLogObjectID = mID;
      }

      //-----------------------------------------------------------------------
      String ICESocketSession::getDebugValueString(bool includeCommaPrefix) const
      {
        AutoRecursiveLock lock(getLock());
        bool firstTime = !includeCommaPrefix;

        return
        Helper::getDebugValue("ice socket session id", string(mID), firstTime) +

        Helper::getDebugValue("state", IICESocketSession::toString(mCurrentState), firstTime) +
        Helper::getDebugValue("last error", 0 != mLastError ? string(mLastError) : String(), firstTime) +
        Helper::getDebugValue("last reason", mLastErrorReason, firstTime) +

        Helper::getDebugValue("subscriptions", mSubscriptions.size() > 0 ? string(mSubscriptions.size()) : String(), firstTime) +
        Helper::getDebugValue("default subscription", mDefaultSubscription ? String("true") : String(), firstTime) +
        Helper::getDebugValue("informed write ready", mInformedWriteReady ? String("true") : String(), firstTime) +

        Helper::getDebugValue("socket subscription", mSocketSubscription ? String("true") : String(), firstTime) +

        Helper::getDebugValue("foundation", mFoundation ? String("true") : String(), firstTime) +

        Helper::getDebugValue("local username frag", mLocalUsernameFrag, firstTime) +
        Helper::getDebugValue("local password", mLocalPassword, firstTime) +
        Helper::getDebugValue("remote username frag", mRemoteUsernameFrag, firstTime) +
        Helper::getDebugValue("remote password", mRemotePassword, firstTime) +

        Helper::getDebugValue("activate timer", mActivateTimer ? String("true") : String(), firstTime) +
        Helper::getDebugValue("keep-alive timer", mKeepAliveTimer ? String("true") : String(), firstTime) +
        Helper::getDebugValue("expecting data timer", mExpectingDataTimer ? String("true") : String(), firstTime) +
        Helper::getDebugValue("step timer", mStepTimer ? String("true") : String(), firstTime) +

        Helper::getDebugValue("control", IICESocket::toString(mControl), firstTime) +
        Helper::getDebugValue("resolver", 0 != mConflictResolver ? string(mConflictResolver) : String(), firstTime) +

        Helper::getDebugValue("nominate request", mNominateRequester ? String("true") : String(), firstTime) +
        (mPendingNominatation ? (String("pending nomination: ") + mPendingNominatation->toDebugString(false)) : String());
        (mNominated ? (String("nominated: ") + mNominated->toDebugString(false)) : String());

        Helper::getDebugValue("last send data", Time() != mLastSentData ? IHelper::timeToString(mLastSentData) : String(), firstTime) +
        Helper::getDebugValue("last activity", Time() != mLastActivity ? IHelper::timeToString(mLastActivity) : String(), firstTime) +

        Helper::getDebugValue("need to notify nominated", mLastNotifiedNominated == mNominated ? String() : String("true"), firstTime) +

        Helper::getDebugValue("alive check requester", mAliveCheckRequester ? String("true") : String(), firstTime) +
        Helper::getDebugValue("last received data/stun", Time() != mLastReceivedDataOrSTUN ? IHelper::timeToString(mLastReceivedDataOrSTUN) : String(), firstTime) +

        Helper::getDebugValue("keep alive (ms)", Duration() != mKeepAliveDuration ? string(mKeepAliveDuration.total_milliseconds()) : String(), firstTime) +
        Helper::getDebugValue("expecting data/stun (ms)", Duration() != mExpectSTUNOrDataWithinDuration ? string(mExpectSTUNOrDataWithinDuration.total_milliseconds()) : String(), firstTime) +
        Helper::getDebugValue("keel alive stun timeout (ms)", Duration() != mKeepAliveSTUNRequestTimeout ? string(mKeepAliveSTUNRequestTimeout.total_milliseconds()) : String(), firstTime) +
        Helper::getDebugValue("backgrounding timeout (ms)", Duration() != mBackgroundingTimeout ? string(mBackgroundingTimeout.total_milliseconds()) : String(), firstTime) +

        Helper::getDebugValue("candidate pairs", mCandidatePairs.size() > 0 ? string(mCandidatePairs.size()) : String(), firstTime) +

        Helper::getDebugValue("updated local candidates", mUpdatedLocalCandidates.size() > 0 ? string(mUpdatedLocalCandidates.size()) : String(), firstTime) +
        Helper::getDebugValue("updated remote candidates", mUpdatedRemoteCandidates.size() > 0 ? string(mUpdatedRemoteCandidates.size()) : String(), firstTime) +

        Helper::getDebugValue("local candidates", mLocalCandidates.size() > 0 ? string(mLocalCandidates.size()) : String(), firstTime) +
        Helper::getDebugValue("remote candidates", mRemoteCandidates.size() > 0 ? string(mRemoteCandidates.size()) : String(), firstTime) +

        Helper::getDebugValue("end of remote candidates flagged", mEndOfRemoteCandidatesFlag ? String("true") : String(), firstTime);
      }

      //-----------------------------------------------------------------------
      void ICESocketSession::cancel()
      {
        AutoRecursiveLock lock(getLock());  // just in case
        if (isShutdown()) {
          ZS_LOG_DEBUG(log("already shutdown"))
          return;
        }

        ZS_LOG_DETAIL(log("cancel"))

        setState(ICESocketSessionState_Shutdown);

        mSubscriptions.clear();
        if (mDefaultSubscription) {
          mDefaultSubscription->cancel();
          mDefaultSubscription.reset();
        }

        if (mSocketSubscription) {
          mSocketSubscription->cancel();
          mSocketSubscription.reset();
        }

        mFoundation.reset();

        IICESocketForICESocketSessionPtr iceSocket = mICESocketWeak.lock();
        if (iceSocket) {
          IICESocketForICESocketSessionProxy::create(mICESocketWeak.lock())->onICESocketSessionClosed(mID);
        }

        mICESocketWeak.reset();

        if (mActivateTimer) {
          mActivateTimer->cancel();
          mActivateTimer.reset();
        }

        if (mKeepAliveTimer) {
          mKeepAliveTimer->cancel();
          mKeepAliveTimer.reset();
        }

        if (mExpectingDataTimer) {
          mExpectingDataTimer->cancel();
          mExpectingDataTimer.reset();
        }

        if (mAliveCheckRequester) {
          mAliveCheckRequester->cancel();
          mAliveCheckRequester.reset();
        }

        if (mStepTimer) {
          mStepTimer->cancel();
          mStepTimer.reset();
        }

        if (mNominateRequester) {
          mNominateRequester->cancel();
          mNominateRequester.reset();
        }
        mPendingNominatation.reset();
        mNominated.reset();

        mLastNotifiedNominated.reset();

        if (mAliveCheckRequester) {
          mAliveCheckRequester->cancel();
          mAliveCheckRequester.reset();
        }

        // scope: we have to completely cancel the old searches...
        {
          for (CandidatePairList::iterator iter = mCandidatePairs.begin(); iter != mCandidatePairs.end(); ++iter)
          {
            CandidatePairPtr pairing = (*iter);
            if (pairing->mRequester) {
              pairing->mRequester->cancel();
              pairing->mRequester.reset();
            }
          }

          // bye-bye old pairs...
          mCandidatePairs.clear();
        }

        mUpdatedLocalCandidates.clear();
        mUpdatedRemoteCandidates.clear();

        mLocalCandidates.clear();
        mRemoteCandidates.clear();
      }

      //-----------------------------------------------------------------------
      void ICESocketSession::setState(ICESocketSessionStates state)
      {
        if (mCurrentState == state) return;

        ZS_LOG_BASIC(log("state changed") + ", old state=" + toString(mCurrentState) + ", new state=" + toString(state))

        mCurrentState = state;

        ICESocketSessionPtr pThis = mThisWeak.lock();

        if (pThis) {
          // inform the delegate of the state change
          mSubscriptions.delegate()->onICESocketSessionStateChanged(pThis, mCurrentState);
        }
      }

      //-----------------------------------------------------------------------
      void ICESocketSession::setError(WORD errorCode, const char *inReason)
      {
        String reason(inReason ? String(inReason) : String());
        if (reason.isEmpty()) {
          reason = IHTTP::toString(IHTTP::toStatusCode(errorCode));
        }

        if (0 != mLastError) {
          ZS_LOG_WARNING(Detail, log("error already set thus ignoring new error") + ", new error=" + string(errorCode) + ", new reason=" + reason + getDebugValueString())
          return;
        }

        get(mLastError) = errorCode;
        mLastErrorReason = reason;

        ZS_LOG_WARNING(Detail, log("error set") + ", code=" + string(mLastError) + ", reason=" + mLastErrorReason + getDebugValueString())
      }
      
      //-----------------------------------------------------------------------
      void ICESocketSession::step()
      {
        if (isShutdown()) {
          ZS_LOG_DEBUG(log("step forwarding to cancel"))
          cancel();
          return;
        }

        ZS_LOG_DEBUG(log("step") + getDebugValueString())

        if (!stepSocket()) goto notify_nominated;
        if (!stepCandidates()) goto notify_nominated;
        if (!stepActivateTimer()) goto notify_nominated;
        if (!stepEndSearch()) goto notify_nominated;
        if (!stepTimer()) goto notify_nominated;
        if (!stepExpectingDataTimer()) goto notify_nominated;
        if (!stepKeepAliveTimer()) goto notify_nominated;
        if (!stepCancelLowerPriority()) goto notify_nominated;
        if (!stepNominate()) goto notify_nominated;

        setState(ICESocketSessionState_Nominated);

      notify_nominated:
        {
          stepNotifyNominated();
        }
      }

      //-----------------------------------------------------------------------
      bool ICESocketSession::stepSocket()
      {
        ZS_LOG_TRACE(log("step socket"))

        IICESocketPtr socket = getSocket();
        if (!socket) {
          setError(IHTTP::HTTPStatusCode_PreconditionFailed, "underlying socket gone");
          cancel();
          return false;
        }

        WORD error = 0;
        String reason;

        IICESocket::ICESocketStates state = socket->getState(&error, &reason);
        switch (state) {
          case IICESocket::ICESocketState_GoingToSleep:
          case IICESocket::ICESocketState_Sleeping:
          {
            if (!mNominated) {
              socket->wakeup();
            }
            break;
          }
          case IICESocket::ICESocketState_ShuttingDown:
          case IICESocket::ICESocketState_Shutdown:
          {
            // socket is self-destructing or is destroyed...
            ZS_LOG_WARNING(Detail, log("ICE socket shutdown") + ", error=" + string(error) + ", reason=" + reason)
            if (0 != error) {
              setError(error, reason);
            }
            cancel();
            return false;
          }
          default: break;
        }

        if (mLocalCandidates.size() < 1) {
          socket->getLocalCandidates(mUpdatedLocalCandidates);
        }

        return true;
      }

      //-----------------------------------------------------------------------
      bool ICESocketSession::stepCandidates()
      {
        ZS_LOG_TRACE(log("step candidates"))

        CandidateList newLocalCandidates;
        CandidateList newRemoteCandidates;

        CandidateList removedCandidates;

        if (mUpdatedLocalCandidates.size() > 0) {
          IICESocket::compare(mLocalCandidates, mUpdatedLocalCandidates, newLocalCandidates, removedCandidates);

          if ((newLocalCandidates.size() > 0) ||
              (removedCandidates.size() > 0)) {
            mLocalCandidates = mUpdatedLocalCandidates;
            mUpdatedLocalCandidates.clear();
          }
        }

        if (mUpdatedRemoteCandidates.size() > 0) {
          removedCandidates.clear();
          IICESocket::compare(mRemoteCandidates, mUpdatedRemoteCandidates, newRemoteCandidates, removedCandidates);

          if ((newRemoteCandidates.size() > 0) ||
              (removedCandidates.size() > 0)) {
            mRemoteCandidates = mUpdatedRemoteCandidates;
            mUpdatedRemoteCandidates.clear();
          }
        }

        if ((newLocalCandidates.size() < 1) &&
            (newRemoteCandidates.size() < 1)) {
          ZS_LOG_TRACE(log("candidates have not changed since last time"))
          return true;
        }

        // scope: assemble the local/remote pairs together (remote to new local candidates)
        {
          for (CandidateList::iterator outer = mRemoteCandidates.begin(); outer != mRemoteCandidates.end(); ++outer) {
            for (CandidateList::const_iterator inner = newLocalCandidates.begin(); inner != newLocalCandidates.end(); ++inner) {
              CandidatePairPtr pairing = CandidatePair::create();

              pairing->mLocal = (*outer);
              pairing->mRemote = (*inner);

              mCandidatePairs.push_back(pairing);
            }
          }
        }

        // scope: assemble the local/remote pairs together (local to new remote candidates)
        {
          for (CandidateList::iterator outer = mLocalCandidates.begin(); outer != mLocalCandidates.end(); ++outer) {
            for (CandidateList::const_iterator inner = newRemoteCandidates.begin(); inner != newRemoteCandidates.end(); ++inner) {
              CandidatePairPtr pairing = CandidatePair::create();

              pairing->mLocal = (*outer);
              pairing->mRemote = (*inner);

              mCandidatePairs.push_back(pairing);
            }
          }
        }

        // sort the list based on priority of the pairs
        if (mControl == IICESocket::ICEControl_Controlling)
          mCandidatePairs.sort(comparePairControlling);
        else
          mCandidatePairs.sort(comparePairControlled);

        // scope: prune the list of non-searchable/redundent candidates
        {
          static const IICESocket::Types searchArray[] = {
            ICESocket::Type_Local,
            ICESocket::Type_ServerReflexive,
            ICESocket::Type_Relayed,
            ICESocket::Type_Unknown
          };

          ULONG totalAdded = 0;

          for (int loop = 0; searchArray[loop] != ICESocket::Type_Unknown; ++loop)
          {
            typedef std::map<IPAddress, CandidateList> ViaLocalIPCandidateList;
            ViaLocalIPCandidateList foundRemotes;

            for (CandidatePairList::iterator canIter = mCandidatePairs.begin(); canIter != mCandidatePairs.end();)
            {
              CandidatePairList::iterator current = canIter;
              ++canIter;

              CandidatePairPtr &pairing = (*current);
              const char *reason = "UNSPECIFIED";

              // only going through one type at a time
              if (pairing->mLocal.mType != searchArray[loop])
                continue;

              // scope: check if candidate should be remoted
              {
                if (totalAdded >= OPENPEER_SERVICES_ICESOCKETSESSION_MAX_REASONABLE_CANDIDATE_PAIR_SEARCHES) {
                  // truncate the list at 100 pairs maximum - RFC says that anything above 100 is unreasonable
                  ZS_LOG_WARNING(Detail, log("too many candidates"))
                  reason = "too many candidates";
                  goto remove_candidate;
                }

                if (ICESocket::Type_ServerReflexive == pairing->mLocal.mType) {
                  // this is server reflixive which can never be sent "from" so elimate it
                  reason = "cannot send from server reflexive";
                  goto remove_candidate;
                }

                IPAddress viaLocalIP = getViaLocalIP(pairing->mLocal);

                ViaLocalIPCandidateList::iterator found = foundRemotes.find(viaLocalIP);
                if (found == foundRemotes.end()) {
                  foundRemotes[viaLocalIP] = CandidateList();
                  found = foundRemotes.find(viaLocalIP);
                }

                CandidateList &useList = (*found).second;

                bool foundType = false;
                for (CandidateList::iterator candIter = useList.begin(); candIter != useList.end(); ++candIter) {
                  Candidate &foundRemoteCandidate = (*candIter);
                  if (foundRemoteCandidate.mIPAddress.isEqualIgnoringIPv4Format(pairing->mRemote.mIPAddress)) {
                    // this is a redundant remote candidate so we will need to eliminate it
                    foundType = true;
                    break;
                  }
                }

                if (foundType) {
                  reason = "remote IP candidate already being searched remotely";
                  goto remove_candidate;
                }

                ++totalAdded;
                useList.push_back(pairing->mRemote);
                continue;
              }

            remove_candidate:
              // scoope: remove candidate
              {
                if ((mNominated == pairing) ||
                    (mPendingNominatation == pairing)) {
                  ZS_LOG_WARNING(Detail, log("cannot remove candidate pair that is nominating/nominated") + pairing->toDebugString())
                  continue;
                }

                // cancel requester
                if (pairing->mRequester) {
                  pairing->mRequester->cancel();
                  pairing->mRequester.reset();
                }

                ZS_LOG_DEBUG(log("removing candidate pair") + ", reason=" + reason + pairing->toDebugString())

                mCandidatePairs.erase(current);
              }
            }
          }
        }

        if (ZS_IS_LOGGING(Debug)) {
          ZS_LOG_DEBUG(log("--- ICE SESSION CANDIDATES START ") + (mControl == IICESocket::ICEControl_Controlling ? "(CONTROLLING) ---" : "(CONTROLLED) ---"))

          for (CandidatePairList::iterator iter = mCandidatePairs.begin(); iter != mCandidatePairs.end(); ++iter) {
            CandidatePairPtr &pairing = (*iter);

            ZS_LOG_DEBUG(log("candidate pair") + ", local ip=" + pairing->mLocal.mIPAddress.string() + " remote=" + pairing->mRemote.mIPAddress.string())
          }
          ZS_LOG_DEBUG(log("--- ICE SESSION CANDIDATES END ---") + ", control=" + (mControl == IICESocket::ICEControl_Controlling ? "CONTROLLING" : "CONTROLLED"))
        }

        return true;
      }

      //-----------------------------------------------------------------------
      bool ICESocketSession::stepActivateTimer()
      {
        bool foundUnsearched = false;

        for (CandidatePairList::iterator iter = mCandidatePairs.begin(); iter != mCandidatePairs.end(); ++iter)
        {
          CandidatePairPtr &pairing = (*iter);

          if (pairing == mNominated) {
            // stop once we are at the nominated point (the rest are lower priority candidates)
            break;
          }

          // is there any need for an activation timer?
          if (pairing->mReceivedResponse) continue;
          if (pairing->mRequester) continue;
          if (pairing->mFailed) continue;

          foundUnsearched = true;
          break;
        }

        ZS_LOG_TRACE(log("step activate timer") + ", needs timer=" + (foundUnsearched ? "true" : "false"))

        if (foundUnsearched) {
          if (mActivateTimer) return true;

          mLastActivity = zsLib::now();
          mActivateTimer = Timer::create(mThisWeak.lock(), Milliseconds(OPENPEER_SERVICES_ICESOCKETSESSION_ACTIVATE_TIMER_IN_MS)); // this will cause candidates to start searching right away

          return true;
        }

        if (!mActivateTimer) return true;

        mActivateTimer->cancel();
        mActivateTimer.reset();
        return true;
      }

      //-----------------------------------------------------------------------
      bool ICESocketSession::stepEndSearch()
      {
        if (!mEndOfRemoteCandidatesFlag) {
          ZS_LOG_TRACE(log("no end of candidates flag set so continue search"))
          return true;
        }
        if (mNominated) {
          ZS_LOG_TRACE(log("already nominated - no reason to end search"))
          return true;
        }

        for (CandidatePairList::iterator iter = mCandidatePairs.begin(); iter != mCandidatePairs.end(); ++iter)
        {
          CandidatePairPtr &pairing = (*iter);

          if (pairing->mFailed) continue;

          ZS_LOG_TRACE(log("found candidate which has not failed thus no reason to end search yet"))
          return true;
        }

        ZS_LOG_ERROR(Detail, log("all candidates have failed"))

        setError(ICESocketSessionShutdownReason_CandidateSearchFailed, "search found no possible candidates to activate");
        cancel();
        return false;
      }

      //-----------------------------------------------------------------------
      bool ICESocketSession::stepTimer()
      {
        ZS_LOG_TRACE(log("step timer") + ", needs timer=" + (mNominated ? "false" : "true"))

        if (!mNominated) {
          if (mStepTimer) return true;

          mLastActivity = zsLib::now();
          mStepTimer = Timer::create(mThisWeak.lock(), Seconds(OPENPEER_SERVICES_ICESOCKETSESSION_STEP_TIMER_IN_SECONDS)); // this will cause candidates to start searching right away

          return true;
        }

        if (!mStepTimer) return true;

        mStepTimer->cancel();
        mStepTimer.reset();
        return true;
      }

      //-----------------------------------------------------------------------
      bool ICESocketSession::stepExpectingDataTimer()
      {
        bool needed =  ((mNominated) && (Duration() != mExpectSTUNOrDataWithinDuration));
        ZS_LOG_TRACE(log("expecting data timer") + ", needs timer=" + (needed ? "true" : "false"))

        if (needed) {
          if (mExpectingDataTimer) return true;

          mLastActivity = zsLib::now();
          mExpectingDataTimer = Timer::create(mThisWeak.lock(), mExpectSTUNOrDataWithinDuration); // this will cause candidates to start searching right away

          return true;
        }

        if (!mExpectingDataTimer) return true;

        mExpectingDataTimer->cancel();
        mExpectingDataTimer.reset();
        return true;
      }

      //-----------------------------------------------------------------------
      bool ICESocketSession::stepKeepAliveTimer()
      {
        bool needed =  ((mNominated) && (Duration() != mKeepAliveDuration));
        ZS_LOG_TRACE(log("keep alive timer") + ", needs timer=" + (needed ? "true" : "false"))

        if (needed) {
          if (mKeepAliveTimer) return true;

          mLastActivity = zsLib::now();
          mKeepAliveTimer = Timer::create(mThisWeak.lock(), mKeepAliveDuration); // this will cause candidates to start searching right away

          return true;
        }

        if (!mKeepAliveTimer) return true;

        mKeepAliveTimer->cancel();
        mKeepAliveTimer.reset();
        return true;
      }
      
      //-----------------------------------------------------------------------
      bool ICESocketSession::stepCancelLowerPriority()
      {
        if (!mNominated) {
          ZS_LOG_TRACE(log("cannot cancel until nominiated"))
          return true;
        }

        bool foundNominated = false;

        for (CandidatePairList::iterator iter = mCandidatePairs.begin(); iter != mCandidatePairs.end(); ++iter)
        {
          CandidatePairPtr &pairing = (*iter);

          if (pairing == mNominated) {
            foundNominated = true;
            continue;
          }

          if (!pairing->mRequester) continue;

          ZS_LOG_DEBUG(log("cancelling requester for candidate") + pairing->toDebugString())

          pairing->mRequester->cancel();
          pairing->mRequester.reset();
        }

        return true;
      }

      //-----------------------------------------------------------------------
      bool ICESocketSession::stepNominate()
      {
        if (mNominateRequester) {
          ZS_LOG_TRACE(log("already nominating"))
          goto set_final_state;
        }

        if (IICESocket::ICEControl_Controlled == mControl)
        {
          if (mNominated) {
            ZS_LOG_TRACE(log("already nominated (any other nominations must come from controlling party)"))
            goto set_final_state;
          }

          ZS_LOG_TRACE(log("waiting for nominatation from remote party"))
          goto set_final_state;
        }

        for (CandidatePairList::iterator iter = mCandidatePairs.begin(); iter != mCandidatePairs.end(); ++iter)
        {
          CandidatePairPtr &pairing = (*iter);

          if (pairing == mNominated) {
            // stop once we are at the nominated point (the rest are lower priority candidates which should never be nominated)
            break;
          }

          if (pairing->mFailed) continue;
          if (!pairing->mReceivedRequest) continue;
          if (!pairing->mReceivedResponse) continue;

          ZS_LOG_DETAIL(log("nominating candidate") + pairing->toDebugString())

          if (mRemotePassword.isEmpty()) {
            ZS_LOG_DEBUG(log("remote password is not set thus this pair can be immediately nominated (i.e. server mode)"))
            // if we never had a username then we were just doing a regular STUN request to the server to detect connectivity

            mNominated = pairing;

            ICESocketPtr socket = mICESocketWeak.lock();
            if (socket) {
              socket->forICESocketSession().addRoute(mThisWeak.lock(), mNominated->mRemote.mIPAddress);
            }

            // we are now connected to this IP address...
            (IWakeDelegateProxy::create(mThisWeak.lock()))->onWake();
            goto set_final_state;
          }

          mPendingNominatation = pairing;

          // this is done to inform the remote party of the nomination since the nominiation process has completed
          STUNPacketPtr request = STUNPacket::createRequest(STUNPacket::Method_Binding);
          fix(request);
          request->mUsername = mRemoteUsernameFrag + ":" + mLocalUsernameFrag;
          request->mPassword = mRemotePassword;
          request->mCredentialMechanism = STUNPacket::CredentialMechanisms_ShortTerm;
          request->mIceControllingIncluded = true;
          request->mIceControlling = mConflictResolver;
          request->mPriorityIncluded = true;
          request->mPriority = mPendingNominatation->mLocal.mPriority;
          request->mUseCandidateIncluded = true;

          // form a new request
          mNominateRequester = ISTUNRequester::create(getAssociatedMessageQueue(), mThisWeak.lock(), mPendingNominatation->mRemote.mIPAddress, request, STUNPacket::RFC_5245_ICE);

          (IWakeDelegateProxy::create(mThisWeak.lock()))->onWake();
          goto set_final_state;
        }

        if (!mNominated) {
          ZS_LOG_TRACE(log("nothing to nominiate yet"))
        }

      set_final_state:
        {
          if (mNominated) return true;

          if (mNominateRequester) {
            setState(ICESocketSessionState_Nominating);
            return false;
          }

          if (mCandidatePairs.size() > 0) {
            for (CandidatePairList::iterator iter = mCandidatePairs.begin(); iter != mCandidatePairs.end(); ++iter)
            {
              CandidatePairPtr &pairing = (*iter);

              if (pairing->mFailed) continue;

              ZS_LOG_TRACE(log("found candidate which has not failed thus still searching"))
              setState(ICESocketSessionState_Searching);
              return false;
            }
            ZS_LOG_DEBUG(log("all known candidates have failed thus search is haulted"))
            setState(ICESocketSessionState_Haulted);
          } else {
            setState(ICESocketSessionState_Prepared);
          }
        }

        return false;
      }

      //-----------------------------------------------------------------------
      void ICESocketSession::stepNotifyNominated()
      {
        if (isShutdown()) return;

        if (mLastNotifiedNominated == mNominated) return;

        ICESocketSessionPtr pThis = mThisWeak.lock();

        mSubscriptions.delegate()->onICESocketSessionNominationChanged(pThis);
        mLastNotifiedNominated = mNominated;
      }

      //-----------------------------------------------------------------------
      void ICESocketSession::switchRole(ICEControls newRole)
      {
        if (isShutdown()) return;
        if (newRole == mControl) return; // role did not switch

        ZS_LOG_WARNING(Detail, log("role conflict detected thus must perform checks from start again"))

        // switch roles now...
        mControl = newRole;

        for (CandidatePairList::iterator iter = mCandidatePairs.begin(); iter != mCandidatePairs.end(); ++iter)
        {
          CandidatePairPtr &pairing = (*iter);

          pairing->mFailed = false;
          pairing->mReceivedRequest = false;
          pairing->mReceivedResponse = false;
          if (pairing->mRequester) {
            pairing->mRequester->cancel();
            pairing->mRequester.reset();
          }
        }

        (IWakeDelegateProxy::create(mThisWeak.lock()))->onWake();
      }

      //-----------------------------------------------------------------------
      bool ICESocketSession::sendTo(
                                    const IICESocket::Candidate &viaLocalCandidate,
                                    const IPAddress &destination,
                                    const BYTE *buffer,
                                    size_t bufferLengthInBytes,
                                    bool isUserData
                                    )
      {
        if (isShutdown()) {
          ZS_LOG_WARNING(Debug, log("cannot send packet as ICE session is closed") + ", candidate: " + viaLocalCandidate.toDebugString(false) + " to ip=" + destination.string() + ", buffer=" + (buffer ? "true" : "false") + ", buffer length=" + string(bufferLengthInBytes) + ", user data=" + (isUserData ? "true" : "false"))
          return false;
        }
        ICESocketPtr socket = mICESocketWeak.lock();
        if (!socket) {
          ZS_LOG_WARNING(Debug, log("cannot send packet as ICE socket is closed") + ", candidate: " + viaLocalCandidate.toDebugString(false) + " to ip=" + destination.string() + ", buffer=" + (buffer ? "true" : "false") + ", buffer length=" + string(bufferLengthInBytes) + ", user data=" + (isUserData ? "true" : "false"))
          return false;
        }

        ZS_LOG_TRACE(log("sending packet") + ", candidate: " + viaLocalCandidate.toDebugString(false) + " to ip=" + destination.string() + ", buffer=" + (buffer ? "true" : "false") + ", buffer length=" + string(bufferLengthInBytes) + ", user data=" + (isUserData ? "true" : "false"))
        return socket->forICESocketSession().sendTo(viaLocalCandidate, destination, buffer, bufferLengthInBytes, isUserData);
      }

      //-----------------------------------------------------------------------
      bool ICESocketSession::canUnfreeze(CandidatePairPtr derivedPairing)
      {
        ZS_THROW_INVALID_ARGUMENT_IF(!derivedPairing)

        AutoRecursiveLock lock(getLock());

        for (CandidatePairList::iterator iter = mCandidatePairs.begin(); iter != mCandidatePairs.end(); ++iter)
        {
          CandidatePairPtr &pairing = (*iter);

          if (pairing->mLocal.mFoundation != derivedPairing->mLocal.mFoundation) continue;  // not from the same foundation
          if (!pairing->mRemote.mIPAddress.isAddressEqualIgnoringIPv4Format(derivedPairing->mRemote.mIPAddress)) continue;

          if (pairing->mFailed) {
            derivedPairing->mFailed = true; // this candidate failed, do not ever search the derived candidate
            return false;
          }
          if (!pairing->mReceivedRequest) return false; // incomplete check, still frozen
          if (!pairing->mReceivedResponse) return false; // incomplete check, still frozen

          ZS_LOG_TRACE(log("foundation is unfozen thus can proceed with activation") + ", foundation: " + pairing->toDebugString(false))

          return true;
        }

        ZS_LOG_DEBUG(log("foundation not found thus can proceed with activation") + ", derived: " + derivedPairing->toDebugString(false))
        return true;
      }
    }

    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    #pragma mark
    #pragma mark IICESocketSession
    #pragma mark

    //-------------------------------------------------------------------------
    String IICESocketSession::toDebugString(IICESocketSessionPtr session, bool includeCommaPrefix)
    {
      return internal::ICESocketSession::toDebugString(session, includeCommaPrefix);
    }

    //-------------------------------------------------------------------------
    const char *IICESocketSession::toString(ICESocketSessionStates state)
    {
      switch (state) {
        case ICESocketSessionState_Pending:    return "Pending";
        case ICESocketSessionState_Prepared:   return "Prepared";
        case ICESocketSessionState_Searching:  return "Searching";
        case ICESocketSessionState_Haulted:    return "Haulted";
        case ICESocketSessionState_Nominating: return "Nominating";
        case ICESocketSessionState_Nominated:  return "Nominated";
        case ICESocketSessionState_Shutdown:   return "Shutdown";
      }
      return "UNDEFINED";
    }

    //-------------------------------------------------------------------------
    const char *IICESocketSession::toString(ICESocketSessionShutdownReasons reason)
    {
      switch (reason) {
        case ICESocketSessionShutdownReason_None:                   return "None";
        case ICESocketSessionShutdownReason_Timeout:                return "Timeout";
        case ICESocketSessionShutdownReason_BackgroundingTimeout:   return "Backgrounding timeout";
        case ICESocketSessionShutdownReason_CandidateSearchFailed:  return "Candidate search failed";
        case ICESocketSessionShutdownReason_DelegateGone:           return "Delegate gone";
      }
      return IHTTP::toString(IHTTP::toStatusCode((WORD)reason));
    }
  }
}
