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

#include <openpeer/services/IDNS.h>
#include <openpeer/services/internal/services_DNS.h>
#include <openpeer/services/internal/services_DNSMonitor.h>
#include <openpeer/services/internal/services_Helper.h>

#include <cryptopp/osrng.h>

#include <zsLib/helpers.h>
#include <zsLib/XML.h>
#include <zsLib/Stringize.h>
#include <zsLib/Log.h>

namespace openpeer { namespace services { ZS_DECLARE_SUBSYSTEM(openpeer_services) } }

namespace openpeer
{
  namespace services
  {
    using CryptoPP::AutoSeededRandomPool;

    typedef std::list<String> StringList;
    typedef std::list<IPAddress> IPAddressList;

    namespace internal
    {
      ZS_DECLARE_CLASS_PTR(DNSQuery)
      ZS_DECLARE_CLASS_PTR(DNSAQuery)
      ZS_DECLARE_CLASS_PTR(DNSAAAAQuery)
      ZS_DECLARE_CLASS_PTR(DNSSRVQuery)
      ZS_DECLARE_CLASS_PTR(DNSAorAAAAQuery)
      ZS_DECLARE_CLASS_PTR(DNSSRVResolverQuery)
      ZS_DECLARE_CLASS_PTR(DNSInstantResultQuery)
      ZS_DECLARE_CLASS_PTR(DNSListQuery)

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark helpers
      #pragma mark

      //-----------------------------------------------------------------------
      static bool srvCompare(const IDNS::SRVResult::SRVRecord &first, const IDNS::SRVResult::SRVRecord &second)
      {
        if (first.mPriority < second.mPriority)
          return true;
        if (first.mPriority > second.mPriority)
          return false;

        DWORD total = (((DWORD)first.mWeight)) + (((DWORD)second.mWeight));

        // they are equal, we have to compare relative weight
        DWORD random = 0;

        AutoSeededRandomPool rng;
        rng.GenerateBlock((BYTE *)&random, sizeof(random));
        if (0 == total)
          return (0 == (random % 2) ? true : false);  // equal chance, 50-50

        random %= total;
        if (random < (((DWORD)first.mWeight)))
          return true;

        return false;
      }

      //-----------------------------------------------------------------------
      static void sortSRV(IDNS::SRVResult &result)
      {
        result.mRecords.sort(srvCompare);
      }

      //-----------------------------------------------------------------------
      static void sortSRV(IDNS::SRVResultPtr result)
      {
        if (!result) return;
        sortSRV(*(result.get()));
      }

      //-----------------------------------------------------------------------
      static void copyToAddressList(
                                    const std::list<IPAddress> &source,
                                    std::list<IPAddress> &dest,
                                    bool includeIPv4 = true,
                                    bool includeIPv6 = true
                                    )
      {
        for(std::list<IPAddress>::const_iterator iter = source.begin(); iter != source.end(); ++iter) {
          if ((*iter).isIPv4()) {
            if (includeIPv4)
              dest.push_back(*iter);
          } else {
            if (includeIPv6)
              dest.push_back(*iter);
          }
        }
      }

      //-----------------------------------------------------------------------
      static void fixDefaultPort(std::list<IPAddress> &result, WORD defaultPort)
      {
        for(std::list<IPAddress>::iterator iter = result.begin(); iter != result.end(); ++iter) {
          if (0 == (*iter).getPort())
            (*iter).setPort(defaultPort);
        }
      }

      //-----------------------------------------------------------------------
      static void fixDefaultPort(IDNS::AResult &result, WORD defaultPort)
      {
        fixDefaultPort(result.mIPAddresses, defaultPort);
      }

      //-----------------------------------------------------------------------
      static void fixDefaultPort(IDNS::AResultPtr result, WORD defaultPort)
      {
        fixDefaultPort(*(result.get()), defaultPort);
      }

      //-----------------------------------------------------------------------
      static void fixDefaultPort(IDNS::SRVResult::SRVRecord &result, WORD defaultPort)
      {
        if (result.mAResult)
          fixDefaultPort(result.mAResult, defaultPort);
        if (result.mAAAAResult)
          fixDefaultPort(result.mAAAAResult, defaultPort);
      }

      //-----------------------------------------------------------------------
      static void fixDefaultPort(IDNS::SRVResult &result, WORD defaultPort)
      {
        if (0 == defaultPort)
          return;

        for (IDNS::SRVResult::SRVRecordList::iterator iter = result.mRecords.begin(); iter != result.mRecords.end(); ++iter) {
          fixDefaultPort(*iter, defaultPort);
        }
      }

      //-----------------------------------------------------------------------
      static void fixDefaultPort(IDNS::SRVResultPtr result, WORD defaultPort)
      {
        fixDefaultPort(*(result.get()), defaultPort);
      }

      //-----------------------------------------------------------------------
      static void tokenize(
                           const String &input,
                           StringList &output,
                           const String &delimiters = " ",
                           const bool includeEmpty = false
                           )
      {
        // so much nicer when something thinks through things for you:
        // http://stackoverflow.com/a/1493195/894732

        String::size_type pos = 0, lastPos = 0;

        while(true)
        {
          pos = input.find_first_of(delimiters, lastPos);

          if (pos == String::npos) {
            pos = input.length();
            if ((pos != lastPos) ||
                (includeEmpty)) {
              output.push_back(std::string(input.data()+lastPos, pos-lastPos));
            }
            break;
          }

          if ((pos != lastPos) ||
              (includeEmpty)) {
            output.push_back(std::string(input.data() + lastPos, pos-lastPos));
          }

          lastPos = pos + 1;
        }
      }

      //-----------------------------------------------------------------------
      static bool isIPAddressList(
                                  const char *name,
                                  WORD defaultPort,
                                  IPAddressList &outIPAddresses
                                  )
      {
        bool found = false;

        try {
          StringList tokenizedList;

          tokenize(String(name ? name : ""), tokenizedList, ",");

          for (StringList::iterator iter = tokenizedList.begin(); iter != tokenizedList.end(); ++iter) {
            const String &value = (*iter);
            if (!IPAddress::isConvertable(value)) return false;

            IPAddress temp(value, defaultPort);
            outIPAddresses.push_back(temp);
            found = true;
          }
        } catch(IPAddress::Exceptions::ParseError &) {
          return false;
        }

        return found;
      }

      //-----------------------------------------------------------------------
      static bool isDNSsList(
                             const char *name,
                             StringList &outList
                             )
      {
        StringList tokenizedList;
        tokenize(String(name ? name : ""), tokenizedList, ",");

        if (tokenizedList.size() > 1) {
          outList = tokenizedList;
          return true;
        }
        return false;
      }

      //-----------------------------------------------------------------------
      static void merge(
                        IDNS::AResultPtr &ioResult,
                        const IDNS::AResultPtr &add
                        )
      {
        if (!ioResult) {
          ioResult = add;
          return;
        }
        if (ioResult->mName.isEmpty()) {
          ioResult->mName = add->mName;
        }
        if (ioResult->mTTL < add->mTTL) {
          ioResult->mTTL = add->mTTL;
        }
        for (IPAddressList::const_iterator iter = add->mIPAddresses.begin(); iter != add->mIPAddresses.end(); ++iter)
        {
          const IPAddress &ip = (*iter);
          ioResult->mIPAddresses.push_back(ip);
        }
      }

      //-----------------------------------------------------------------------
      static void merge(
                        IDNS::SRVResultPtr &ioResult,
                        const IDNS::SRVResultPtr &add
                        )
      {
        typedef IDNS::SRVResult::SRVRecord SRVRecord;
        typedef IDNS::SRVResult::SRVRecordList SRVRecordList;

        if (!ioResult) {
          ioResult = add;
          return;
        }
        if (ioResult->mName.isEmpty()) {
          ioResult->mName = add->mName;
        }
        if (ioResult->mService.isEmpty()) {
          ioResult->mService = add->mService;
        }
        if (ioResult->mProtocol.isEmpty()) {
          ioResult->mProtocol = add->mProtocol;
        }
        if (ioResult->mTTL < add->mTTL) {
          ioResult->mTTL = add->mTTL;
        }

        for (SRVRecordList::const_iterator iter = add->mRecords.begin(); iter != add->mRecords.end(); ++iter)
        {
          const SRVRecord &record = (*iter);
          ioResult->mRecords.push_back(record);
        }
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark DNSQuery
      #pragma mark

      class DNSQuery : public SharedRecursiveLock,
                       public IDNSQuery
      {
      protected:
        //---------------------------------------------------------------------
        virtual void onAResult(IDNS::AResultPtr result) {}

        //---------------------------------------------------------------------
        virtual void onAAAAResult(IDNS::AAAAResultPtr result) {}

        //---------------------------------------------------------------------
        virtual void onSRVResult(IDNS::SRVResultPtr result) {}

      protected:
        //---------------------------------------------------------------------
        // At all times the object reference to a DNSQuery is the caller which
        // created the query in the first place so the DNSQuery objects
        // destruction causes the corresponding outstanding DNSQuery object to
        // get destroyed. The monitor does not need to maintain a strong
        // reference to the query object, however, when there is an oustanding
        // dns_query object at any time the result could come back with a
        // void * which needs to be cast to our object. Normally this would
        // require a strong reference to exist to the object except we use
        // this indirection where a strong reference is maintained to that
        // object with a weak reference to the real object thus the DNSQuery
        // can be cancelled by just deleted the DNSQuery object.
        ZS_DECLARE_CLASS_PTR(DNSIndirectReference)

        //---------------------------------------------------------------------
        //---------------------------------------------------------------------
        //---------------------------------------------------------------------
        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark DNSQuery::DNSIndirectReference
        #pragma mark

        class DNSIndirectReference : public DNSMonitor::IResult {
        public:
          //-------------------------------------------------------------------
          static DNSIndirectReferencePtr create(DNSQueryPtr query) {
            DNSIndirectReferencePtr pThis(new DNSIndirectReference);
            pThis->mThisWeak = pThis;
            pThis->mMonitor = DNSMonitor::singleton();
            pThis->mOuter = query;
            pThis->mQueryID = 0;
            return pThis;
          }

          //-------------------------------------------------------------------
          ~DNSIndirectReference()
          {
            mThisWeak.reset();
            cancel();
          }

          //-------------------------------------------------------------------
          #pragma mark
          #pragma mark DNSQuery::DNSIndirectReference => DNSMonitor::IResult
          #pragma mark

          //-------------------------------------------------------------------
          // when cleaning out a strong reference to yourself, you must ensure
          // to keep the reference alive in the stack so the object is
          // destroyed after the mThis variable is reset
          virtual PUID getID() const {return mID;}

          //-------------------------------------------------------------------
          virtual void cancel()
          {
            DNSMonitorPtr monitor = mMonitor.lock();
            if (!monitor) return;

            monitor->cancel(mQueryID, mThisWeak.lock());
          }

          //-------------------------------------------------------------------
          virtual void setQueryID(QueryID queryID)
          {
            mQueryID = queryID;
          }

          //-------------------------------------------------------------------
          virtual void onCancel()
          {
            DNSQueryPtr outer = mOuter.lock();
            if (!outer)
              return;

            outer->cancel();
            mOuter.reset();
          }

          //-------------------------------------------------------------------
          virtual void onAResult(IDNS::AResultPtr result) {
            DNSQueryPtr outer = mOuter.lock();
            if (!outer)
              return;

            outer->onAResult(IDNS::cloneA(result));
            mOuter.reset();
          }

          //-------------------------------------------------------------------
          virtual void onAAAAResult(IDNS::AAAAResultPtr result) {
            DNSQueryPtr outer = mOuter.lock();
            if (!outer)
              return;

            outer->onAAAAResult(IDNS::cloneAAAA(result));
            mOuter.reset();
          }

          //-------------------------------------------------------------------
          virtual void onSRVResult(IDNS::SRVResultPtr result) {
            DNSQueryPtr outer = mOuter.lock();
            if (!outer)
              return;

            result = IDNS::cloneSRV(result);
            sortSRV(result);

            outer->onSRVResult(result);
            mOuter.reset();
          }

        public:
          //-------------------------------------------------------------------
          #pragma mark
          #pragma mark DNSQuery::DNSIndirectReference => (data)
          #pragma mark

          PUID mID;
          DNSIndirectReferenceWeakPtr mThisWeak;
          DNSQueryWeakPtr mOuter;

          DNSMonitorWeakPtr mMonitor;
          QueryID mQueryID;
        };

      protected:
        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark DNSQuery => (internal/derived)
        #pragma mark

        //---------------------------------------------------------------------
        DNSQuery(
                 DNSMonitorPtr monitor,
                 IDNSDelegatePtr delegate
                 ) :
          mMonitor(monitor),
          SharedRecursiveLock(monitor ? *monitor : SharedRecursiveLock::create()),
          mObjectName("DNSQuery")
        {
          ZS_THROW_INVALID_USAGE_IF(!delegate)
          IMessageQueuePtr queue = Helper::getServiceQueue();
          if (!queue) {
            ZS_THROW_BAD_STATE_MSG_IF(!queue, "The service thread was not created")
          }

          mDelegate = IDNSDelegateProxy::createWeak(delegate);
          mMonitor = DNSMonitor::singleton();
        }

        //---------------------------------------------------------------------
        ~DNSQuery() { mThisWeak.reset(); cancel(); }

        //---------------------------------------------------------------------
        Log::Params log(const char *message) const
        {
          ElementPtr objectEl = Element::create(mObjectName);
          IHelper::debugAppend(objectEl, "id", mID);
          return Log::Params(message, objectEl);
        }

        //---------------------------------------------------------------------
        virtual void abortEarly()
        {
          AutoRecursiveLock lock(*this);

          cancel();

          if (!mDelegate) return;

          DNSQueryPtr pThis = mThisWeak.lock();
          if (!pThis) return;

          try {
            mDelegate->onLookupCompleted(pThis);
          } catch (IDNSDelegateProxy::Exceptions::DelegateGone &) {
          }

          mDelegate.reset();
        }

        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark DNSQuery => IDNSQuery
        #pragma mark

        //---------------------------------------------------------------------
        virtual PUID getID() const {return mID;}

        //---------------------------------------------------------------------
        virtual void cancel()
        {
          AutoRecursiveLock lock(*this);

          if (mQuery) {
            mQuery->cancel();
            mQuery.reset();
          }
        }

        //---------------------------------------------------------------------
        virtual bool hasResult() const {return mA || mAAAA || mSRV;}

        //---------------------------------------------------------------------
        virtual bool isComplete() const {return !mQuery;}

        //---------------------------------------------------------------------
        virtual AResultPtr getA() const {return IDNS::cloneA(mA);}

        //---------------------------------------------------------------------
        virtual AAAAResultPtr getAAAA() const {return IDNS::cloneAAAA(mAAAA);}

        //---------------------------------------------------------------------
        virtual SRVResultPtr getSRV() const {return IDNS::cloneSRV(mSRV);}

      protected:
        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark DNSQuery => (internal)
        #pragma mark

        //---------------------------------------------------------------------
        void done()
        {
          mQuery.reset();
        }

      protected:
        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark DNSQuery => (data)
        #pragma mark

        DNSMonitorPtr mMonitor;
        AutoPUID mID;
        DNSQueryWeakPtr mThisWeak;
        const char *mObjectName;

        IDNSDelegatePtr mDelegate;

        DNSIndirectReferencePtr mQuery;

        AResultPtr mA;
        AAAAResultPtr mAAAA;
        SRVResultPtr mSRV;
      };

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark DNSAQuery
      #pragma mark

      class DNSAQuery : public DNSQuery
      {
      protected:
        DNSAQuery(IDNSDelegatePtr delegate, const char *name) :
          DNSQuery(DNSMonitor::singleton(), delegate),
          mName(name)
        {
          mObjectName = "DNSAQuery";
        }

      public:
        //---------------------------------------------------------------------
        static DNSAQueryPtr create(IDNSDelegatePtr delegate, const char *name)
        {
          DNSAQueryPtr pThis(new DNSAQuery(delegate, name));
          pThis->mThisWeak = pThis;
          pThis->mMonitor = DNSMonitor::singleton();
          pThis->mQuery = DNSIndirectReference::create(pThis);

          if (pThis->mMonitor) {
            pThis->mMonitor->submitAQuery(name, 0, pThis->mQuery);
          } else {
            pThis->abortEarly();
          }

          return pThis;
        }

      protected:
        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark DNSAQuery => IDNSQuery
        #pragma mark

        //---------------------------------------------------------------------
        virtual void onAResult(IDNS::AResultPtr result)
        {
          AutoRecursiveLock lock(*this);
          if (!mQuery) {
            ZS_LOG_WARNING(Detail, log("A record lookup was cancelled before result arrived") + ZS_PARAM("name", mName))
            return;
          }
          done();

          mA = result;

          if (mA) {
            for (IDNS::AResult::IPAddressList::iterator iter = mA->mIPAddresses.begin(); iter != mA->mIPAddresses.end(); ++iter)
            {
              IPAddress &ipAddress = (*iter);
              ZS_LOG_DEBUG(log("A record found") + ZS_PARAM("ip", ipAddress.string()))
            }
          } else {
            ZS_LOG_DEBUG(log("A record lookup failed") + ZS_PARAM("name", mName))
          }

          try {
            mDelegate->onLookupCompleted(mThisWeak.lock());
          } catch (IDNSDelegateProxy::Exceptions::DelegateGone &) {
          }
        }

      protected:
        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark DNSAQuery => (data)
        #pragma mark

        String mName;
      };

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark DNSAAAAQuery
      #pragma mark

      class DNSAAAAQuery : public DNSQuery
      {
      protected:
        DNSAAAAQuery(IDNSDelegatePtr delegate, const char *name) :
          DNSQuery(DNSMonitor::singleton(), delegate),
          mName(name)
        {
          mObjectName = "DNSAAAAQuery";
        }

      public:
        //---------------------------------------------------------------------
        static DNSAAAAQueryPtr create(IDNSDelegatePtr delegate, const char *name)
        {
          DNSAAAAQueryPtr pThis(new DNSAAAAQuery(delegate, name));
          pThis->mThisWeak = pThis;
          pThis->mMonitor = DNSMonitor::singleton();
          pThis->mQuery = DNSIndirectReference::create(pThis);

          if (pThis->mMonitor) {
            pThis->mMonitor->submitAAAAQuery(name, 0, pThis->mQuery);
          } else {
            pThis->abortEarly();
          }

          return pThis;
        }

      protected:
        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark DNSAAAAQuery => IDNSQuery
        #pragma mark

        //---------------------------------------------------------------------
        virtual void onAAAAResult(IDNS::AAAAResultPtr result)
        {
          AutoRecursiveLock lock(*this);
          if (!mQuery) {
            ZS_LOG_WARNING(Detail, log("AAAA was cancelled before result arrived") + ZS_PARAM("name", mName))
            return;
          }
          done();

          mAAAA = result;

          if (mAAAA) {
            for (IDNS::AResult::IPAddressList::iterator iter = mAAAA->mIPAddresses.begin(); iter != mAAAA->mIPAddresses.end(); ++iter)
            {
              IPAddress &ipAddress = (*iter);
              ZS_LOG_DEBUG(log("AAAA record found") + ZS_PARAM("ip", ipAddress.string()))
            }
          } else {
            ZS_LOG_DEBUG(log("AAAA record lookup failed") + ZS_PARAM("name", mName))
          }

          try {
            mDelegate->onLookupCompleted(mThisWeak.lock());
          }
          catch (IDNSDelegateProxy::Exceptions::DelegateGone &) {
          }
        }

      protected:
        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark DNSAAAAQuery => (data)
        #pragma mark

        String mName;
      };

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark DNSSRVQuery
      #pragma mark

      class DNSSRVQuery : public DNSQuery
      {
      protected:
        DNSSRVQuery(
                    IDNSDelegatePtr delegate,
                    const char *name,
                    const char *service,
                    const char *protocol
                    ) :
          DNSQuery(DNSMonitor::singleton(), delegate),
          mName(name),
          mService(service),
          mProtocol(protocol)
        {
          mObjectName = "DNSSRVQuery";
        }

      public:
        //---------------------------------------------------------------------
        static DNSSRVQueryPtr create(
                                     IDNSDelegatePtr delegate,
                                     const char *name,
                                     const char *service,
                                     const char *protocol
                                     )
        {
          DNSSRVQueryPtr pThis(new DNSSRVQuery(delegate, name, service, protocol));
          pThis->mThisWeak = pThis;
          pThis->mMonitor = DNSMonitor::singleton();
          pThis->mQuery = DNSIndirectReference::create(pThis);

          if (pThis->mMonitor) {
            pThis->mMonitor->submitSRVQuery(name, service, protocol, 0, pThis->mQuery);
          } else {
            pThis->abortEarly();
          }
          return pThis;
        }

      protected:
        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark DNSSRVQuery => IDNSQuery
        #pragma mark

        //---------------------------------------------------------------------
        virtual void onSRVResult(IDNS::SRVResultPtr result)
        {
          AutoRecursiveLock lock(*this);
          if (!mQuery) {
            ZS_LOG_WARNING(Detail, log("SRV record lookup was cancelled before result arrived") + ZS_PARAM("name", mName) + ZS_PARAM("service", mService) + ZS_PARAM("protocol", mProtocol))
            return;
          }
          done();

          mSRV = result;
          if (mSRV) {
            ZS_LOG_DEBUG(log("SRV completed") + ZS_PARAM("name", mName) + ZS_PARAM("service", mService) + ZS_PARAM("protocol", mProtocol))
            for (IDNS::SRVResult::SRVRecordList::iterator iter = mSRV->mRecords.begin(); iter != mSRV->mRecords.end(); ++iter)
            {
              SRVResult::SRVRecord &srvRecord = (*iter);
              ZS_LOG_DEBUG(log("SRV record found") + ZS_PARAM("name", srvRecord.mName) + ZS_PARAM("port", srvRecord.mPort) + ZS_PARAM("priority", srvRecord.mPriority) + ZS_PARAM("weight", srvRecord.mWeight))
            }
          } else {
            ZS_LOG_DEBUG(log("SRV record lookup failed") + ZS_PARAM("name", mName) + ZS_PARAM("service", mService) + ZS_PARAM("protocol", mProtocol))
          }

          try {
            mDelegate->onLookupCompleted(mThisWeak.lock());
          }
          catch (IDNSDelegateProxy::Exceptions::DelegateGone &) {
          }
        }

      protected:
        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark DNSSRVQuery (data)
        #pragma mark

        String mName;
        String mService;
        String mProtocol;
      };


      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark DNSAorAAAAQuery
      #pragma mark

      class DNSAorAAAAQuery : public MessageQueueAssociator,
                              public IDNSQuery,
                              public IDNSDelegate
      {
      protected:
        //---------------------------------------------------------------------
        DNSAorAAAAQuery(
                        IMessageQueuePtr queue,
                        IDNSDelegatePtr delegate
                        ) :
          MessageQueueAssociator(queue),
          mID(zsLib::createPUID()),
          mDelegate(IDNSDelegateProxy::createWeak(queue, delegate))
        {
        }

        //---------------------------------------------------------------------
        void init(const char *name)
        {
          AutoRecursiveLock lock(mLock);
          mALookup = IDNS::lookupA(mThisWeak.lock(), name);
          mAAAALookup = IDNS::lookupAAAA(mThisWeak.lock(), name);
        }

        //---------------------------------------------------------------------
        void report()
        {
          if (mALookup) {
            if (!mALookup->isComplete()) return;
          }
          if (mAAAALookup) {
            if (!mAAAALookup->isComplete()) return;
          }

          if (!mDelegate) return;

          try {
            mDelegate->onLookupCompleted(mThisWeak.lock());
          } catch(IDNSDelegateProxy::Exceptions::DelegateGone &) {
          }

          mDelegate.reset();
        }

      public:
        //---------------------------------------------------------------------
        static DNSAorAAAAQueryPtr create(
                                         IDNSDelegatePtr delegate,
                                         const char *name
                                         )
        {
          ZS_THROW_INVALID_USAGE_IF(!delegate)
          IMessageQueuePtr queue = Helper::getServiceQueue();
          ZS_THROW_BAD_STATE_IF(!queue)

          DNSAorAAAAQueryPtr pThis(new DNSAorAAAAQuery(queue, delegate));
          pThis->mThisWeak = pThis;
          pThis->init(name);
          return pThis;
        }

        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark DNSAorAAAAQuery => IDNSQuery
        #pragma mark

        //---------------------------------------------------------------------
        virtual PUID getID() const {return mID;}

        //---------------------------------------------------------------------
        virtual void cancel()
        {
          AutoRecursiveLock lock(mLock);

          if (mALookup)
            mALookup->cancel();

          if (mAAAALookup)
            mAAAALookup->cancel();

          // clear out all requests
          mDelegate.reset();
          mALookup.reset();
          mAAAALookup.reset();
        }

        //---------------------------------------------------------------------
        virtual bool hasResult() const
        {
          AutoRecursiveLock lock(mLock);

          bool result = false;
          if (mALookup) {
            result = result || mALookup->hasResult();
          }
          if (mAAAALookup) {
            result = result || mAAAALookup->hasResult();
          }
          return result;
        }

        //---------------------------------------------------------------------
        virtual bool isComplete() const {
          AutoRecursiveLock lock(mLock);

          bool complete = true;
          if (mALookup) {
            complete = complete && mALookup->isComplete();
          }
          if (mAAAALookup) {
            complete = complete && mAAAALookup->isComplete();
          }
          return complete;
        }

        //---------------------------------------------------------------------
        virtual AResultPtr getA() const
        {
          AutoRecursiveLock lock(mLock);

          if (!mALookup) return AResultPtr();
          return mALookup->getA();
        }

        //---------------------------------------------------------------------
        virtual AAAAResultPtr getAAAA() const
        {
          AutoRecursiveLock lock(mLock);

          if (!mAAAALookup) return AAAAResultPtr();
          return mAAAALookup->getAAAA();
        }

        //---------------------------------------------------------------------
        virtual SRVResultPtr getSRV() const {return SRVResultPtr();}

        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark DNSAorAAAAQuery => IDNSDelegate
        #pragma mark

        //---------------------------------------------------------------------
        virtual void onLookupCompleted(IDNSQueryPtr query)
        {
          AutoRecursiveLock lock(mLock);
          report();
        }

      protected:
        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark DNSAorAAAAQuery => (data)
        #pragma mark

        mutable RecursiveLock mLock;
        PUID mID;

        DNSAorAAAAQueryWeakPtr mThisWeak;
        IDNSDelegatePtr mDelegate;

        IDNSQueryPtr mALookup;
        IDNSQueryPtr mAAAALookup;
      };

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark DNSSRVResolverQuery
      #pragma mark

      class DNSSRVResolverQuery : public MessageQueueAssociator,
                                  public IDNSQuery,
                                  public IDNSDelegate
      {
      protected:
        DNSSRVResolverQuery(
                            IMessageQueuePtr queue,
                            IDNSDelegatePtr delegate,
                            const char *name,
                            const char *service,
                            const char *protocol,
                            WORD defaultPort,
                            WORD defaultPriority,
                            WORD defaultWeight,
                            IDNS::SRVLookupTypes lookupType
                            ) :
          MessageQueueAssociator(queue),
          mDelegate(IDNSDelegateProxy::createWeak(queue, delegate)),
          mOriginalName(name),
          mOriginalService(service),
          mOriginalProtocol(protocol),
          mDefaultPort(defaultPort),
          mDefaultPriority(defaultPriority),
          mDefaultWeight(defaultWeight),
          mLookupType(lookupType)
        {
          ZS_LOG_TRACE(log("created"))
        }

        //---------------------------------------------------------------------
        void init()
        {
          AutoRecursiveLock lock(mLock);
          mSRVLookup = IDNS::lookupSRV(
                                       mThisWeak.lock(),
                                       mOriginalName,
                                       mOriginalService,
                                       mOriginalProtocol,
                                       mDefaultPort,
                                       mDefaultPriority,
                                       mDefaultWeight,
                                       IDNS::SRVLookupType_LookupOnly
                                       );  // do an actual SRV DNS lookup which will not resolve the A or AAAA records

          // SRV might fail but perhaps we can do a backup lookup in parallel...
          IDNSQueryPtr backupQuery;
          if (IDNS::SRVLookupType_FallbackToALookup == (mLookupType & IDNS::SRVLookupType_FallbackToALookup)) {
            if (IDNS::SRVLookupType_FallbackToAAAALookup == (mLookupType & IDNS::SRVLookupType_FallbackToAAAALookup)) {
              backupQuery = IDNS::lookupAorAAAA(mThisWeak.lock(), mOriginalName);
            } else {
              backupQuery = IDNS::lookupA(mThisWeak.lock(), mOriginalName);
            }
          } else {
            if (IDNS::SRVLookupType_FallbackToAAAALookup == (mLookupType & IDNS::SRVLookupType_FallbackToAAAALookup))
              backupQuery = IDNS::lookupAAAA(mThisWeak.lock(), mOriginalName);
          }

          mBackupLookup = backupQuery;
        }

      public:
        //---------------------------------------------------------------------
        ~DNSSRVResolverQuery()
        {
          mThisWeak.reset();
          ZS_LOG_TRACE(log("destroyed"))
        }

        //---------------------------------------------------------------------
        static DNSSRVResolverQueryPtr create(
                                             IDNSDelegatePtr delegate,
                                             const char *name,
                                             const char *service,
                                             const char *protocol,
                                             WORD defaultPort,
                                             WORD defaultPriority,
                                             WORD defaultWeight,
                                             IDNS::SRVLookupTypes lookupType
                                             )
        {
          ZS_THROW_INVALID_USAGE_IF(!delegate)
          IMessageQueuePtr queue = Helper::getServiceQueue();
          ZS_THROW_INVALID_USAGE_IF(!queue)

          DNSSRVResolverQueryPtr pThis(new DNSSRVResolverQuery(queue, delegate, name, service, protocol, defaultPort, defaultPriority, defaultWeight, lookupType));
          pThis->mThisWeak = pThis;
          pThis->init();
          return pThis;
        }

        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark DNSSRVResolverQuery => IDNSQuery
        #pragma mark

        //---------------------------------------------------------------------
        virtual PUID getID() const {return mID;}

        //---------------------------------------------------------------------
        virtual bool hasResult() const
        {
          AutoRecursiveLock lock(mLock);
          if (!isComplete()) return false;
          return (bool)mSRVResult;
        }

        //---------------------------------------------------------------------
        virtual bool isComplete() const
        {
          AutoRecursiveLock lock(mLock);
          return mDidComplete;

          // all the sub resolvers must be complete...
          for (ResolverList::const_iterator iter = mResolvers.begin(); iter != mResolvers.end(); ++iter) {
            if (*iter)
              return false;   // at least one resolver is still active
          }

          if (mSRVLookup) {
            if (!mSRVLookup->isComplete()) {
              return false;
            }
          }

          if (mBackupLookup) {
            if (!mBackupLookup->isComplete()) {
              return false;
            }
          }

          return true;
        }

        //---------------------------------------------------------------------
        virtual AResultPtr getA() const {return AResultPtr();}

        //---------------------------------------------------------------------
        virtual AAAAResultPtr getAAAA() const {return AAAAResultPtr();}

        //---------------------------------------------------------------------
        virtual SRVResultPtr getSRV() const
        {
          AutoRecursiveLock lock(mLock);
          return IDNS::cloneSRV(mSRVResult);
        }

        //---------------------------------------------------------------------
        virtual void cancel()
        {
          AutoRecursiveLock lock(mLock);

          get(mDidComplete) = true;

          if (mSRVLookup)
            mSRVLookup->cancel();
          if (mBackupLookup)
            mBackupLookup->cancel();

          ResolverList::iterator iter = mResolvers.begin();
          for (; iter != mResolvers.end(); ++iter) {
            if (*iter)
              (*iter)->cancel();
            (*iter).reset();
          }

          mResolvers.clear();
        }

        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark DNSSRVResolverQuery => IDNSDelegate
        #pragma mark

        //---------------------------------------------------------------------
        virtual void onLookupCompleted(IDNSQueryPtr query)
        {
          AutoRecursiveLock lock(mLock);
          step();
        }

      protected:
        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark DNSSRVResolverQuery => (internal)
        #pragma mark

        //---------------------------------------------------------------------
        void step()
        {
          ZS_LOG_TRACE(log("step") + toDebug())

          if (!stepHandleSRVCompleted()) return;
          if (!stepHandleBackupCompleted()) return;
          if (!stepHandleResolversCompleted()) return;

          ZS_LOG_DEBUG(log("step complete") + toDebug())

          get(mDidComplete) = true;

          report();
        }

        //---------------------------------------------------------------------
        bool stepHandleSRVCompleted()
        {
          if (mSRVResult) {
            ZS_LOG_TRACE(log("already have a result"))
            return true;
          }

          if (!mSRVLookup) {
            ZS_LOG_ERROR(Detail, debug("primary lookup failed to create interface"))
            return true;
          }

          if (!mSRVLookup->isComplete()) {
            ZS_LOG_TRACE(log("waiting for SRV to complete"))
            return false;
          }

          if (!mSRVLookup->hasResult()) {
            ZS_LOG_TRACE(log("SRV lookup failed to resolve (will check if there is a backup)"))
            return true;
          }

          mSRVResult = mSRVLookup->getSRV();

          ZS_LOG_DEBUG(log("SRV result found") + toDebug())

          // the SRV resolved so now we must do a lookup for each SRV result
          for (IDNS::SRVResult::SRVRecordList::iterator iter = mSRVResult->mRecords.begin(); iter != mSRVResult->mRecords.end(); ++iter) {
            // first we should check if this is actually an IP address
            if (IPAddress::isConvertable((*iter).mName)) {
              IPAddress temp((*iter).mName, (*iter).mPort);
              IDNS::AResultPtr ipResult(new IDNS::AResult);

              ipResult->mName = (*iter).mName;
              ipResult->mTTL = mSRVResult->mTTL;
              ipResult->mIPAddresses.push_back(temp);

              if (temp.isIPv4()) {
                (*iter).mAResult = ipResult;
              } else {
                (*iter).mAAAAResult = ipResult;
              }

              IDNSQueryPtr subQuery;
              mResolvers.push_back(subQuery); // push back an empty resolver since the list must be exactly the same length but the resovler will be treated as if it has completed
              continue; // we don't need to go any futher
            }

            IDNSQueryPtr subQuery;
            if (IDNS::SRVLookupType_AutoLookupA == (mLookupType & IDNS::SRVLookupType_AutoLookupA)) {
              if (IDNS::SRVLookupType_AutoLookupAAAA == (mLookupType & IDNS::SRVLookupType_AutoLookupAAAA)) {
                subQuery = IDNS::lookupAorAAAA(mThisWeak.lock(), (*iter).mName);
              } else {
                subQuery = IDNS::lookupA(mThisWeak.lock(), (*iter).mName);
              }
            } else {
              subQuery = IDNS::lookupAAAA(mThisWeak.lock(), (*iter).mName);
            }
            mResolvers.push_back(subQuery);
          }

          return true;
        }

        //---------------------------------------------------------------------
        bool stepHandleBackupCompleted()
        {
          if (mSRVResult) {
            ZS_LOG_TRACE(log("already have a result"))
            return true;
          }

          if (!mBackupLookup) {
            ZS_LOG_DEBUG(log("back-up query was not used"))
            return true;
          }

          if (!mBackupLookup->isComplete()) {
            ZS_LOG_TRACE(log("waiting for backup query to resolve"))
            return false;
          }

          if (!mBackupLookup->hasResult()) {
            ZS_LOG_WARNING(Trace, log("SRV and backup failed to resolve"))
            return true;
          }

          // we didn't have an SRV result but now we will fake one
          IDNS::SRVResultPtr data(new IDNS::SRVResult);

          AResultPtr resultA = mBackupLookup->getA();
          AAAAResultPtr resultAAAA = mBackupLookup->getAAAA();

          data->mName = mOriginalName;
          data->mService = mOriginalService;
          data->mProtocol = mOriginalProtocol;
          data->mTTL = (resultA ? resultA->mTTL : resultAAAA->mTTL);

          IDNS::SRVResult::SRVRecord srvRecord;
          srvRecord.mPriority = mDefaultPriority;
          srvRecord.mWeight = mDefaultWeight;
          srvRecord.mPort = 0;
          srvRecord.mName = mOriginalName;
          srvRecord.mAResult = resultA;
          srvRecord.mAAAAResult = resultAAAA;

          fixDefaultPort(srvRecord, mDefaultPort);

          ZS_LOG_DEBUG(log("DNS A/AAAAA converting to SRV record") + ZS_PARAM("name", srvRecord.mName) + ZS_PARAM("port", srvRecord.mPort) + ZS_PARAM("priority", srvRecord.mPriority) + ZS_PARAM("weight", srvRecord.mWeight))

          data->mRecords.push_back(srvRecord);
          mSRVResult = data;

          return true;
        }

        //---------------------------------------------------------------------
        virtual bool stepHandleResolversCompleted()
        {
          if (mResolvers.size() < 1) {
            ZS_LOG_TRACE(log("no resolvers found"))
            return true;
          }

          if (!mSRVResult) {
            ZS_LOG_TRACE(log("no SRV result found"))
            return true;
          }

          IDNS::SRVResult::SRVRecordList::iterator recIter = mSRVResult->mRecords.begin();
          ResolverList::iterator resIter = mResolvers.begin();
          for (; recIter != mSRVResult->mRecords.end() && resIter != mResolvers.end(); ++recIter, ++resIter) {
            IDNS::SRVResult::SRVRecord &record = (*recIter);
            IDNSQueryPtr &query = (*resIter);

            if (query) {
              if (!query->isComplete()) {
                ZS_LOG_TRACE(log("waiting on at least one resolver to complete"))
                return false;
              }

              record.mAResult = query->getA();
              record.mAAAAResult = query->getAAAA();

              fixDefaultPort(record, record.mPort);

              query.reset();
            }
          }

          ZS_LOG_TRACE(log("all resolvers are complete"))
          return true;
        }

        //---------------------------------------------------------------------
        void report()
        {
          if (!mDelegate) return;

          mResolvers.clear();

          try {
            mDelegate->onLookupCompleted(mThisWeak.lock());
          } catch(IDNSDelegateProxy::Exceptions::DelegateGone &) {
          }
          mDelegate.reset();
        }

        //---------------------------------------------------------------------
        Log::Params log(const char *message) const
        {
          ElementPtr objectEl = Element::create("DNSSRVResolverQuery");
          IHelper::debugAppend(objectEl, "id", mID);
          return Log::Params(message, objectEl);
        }

        //---------------------------------------------------------------------
        Log::Params debug(const char *message) const
        {
          return Log::Params(message, toDebug());
        }

        //---------------------------------------------------------------------
        virtual ElementPtr toDebug() const
        {
          AutoRecursiveLock lock(mLock);

          ElementPtr resultEl = Element::create("DNSSRVResolverQuery");
          IHelper::debugAppend(resultEl, "id", mID);
          IHelper::debugAppend(resultEl, "completed", mDidComplete);

          IHelper::debugAppend(resultEl, "name", mOriginalName);
          IHelper::debugAppend(resultEl, "service", mOriginalService);
          IHelper::debugAppend(resultEl, "protocol", mOriginalProtocol);

          IHelper::debugAppend(resultEl, "default port", mDefaultPort);
          IHelper::debugAppend(resultEl, "default priority", mDefaultPriority);
          IHelper::debugAppend(resultEl, "default weight", mDefaultWeight);

          IHelper::debugAppend(resultEl, "SRV lookup", (bool)mSRVLookup);
          IHelper::debugAppend(resultEl, "backup lookup", (bool)mBackupLookup);

          IHelper::debugAppend(resultEl, "SRV result", (bool)mSRVResult);

          IHelper::debugAppend(resultEl, "lookup type", mLookupType);

          IHelper::debugAppend(resultEl, "resolvers", mResolvers.size());
          return resultEl;
        }

      protected:
        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark DNSSRVResolverQuery => (data)
        #pragma mark

        mutable RecursiveLock mLock;
        AutoPUID mID;

        DNSSRVResolverQueryWeakPtr mThisWeak;
        IDNSDelegatePtr mDelegate;

        AutoBool mDidComplete;

        String mOriginalName;
        String mOriginalService;
        String mOriginalProtocol;

        WORD mDefaultPort;
        WORD mDefaultPriority;
        WORD mDefaultWeight;

        IDNSQueryPtr mSRVLookup;
        IDNSQueryPtr mBackupLookup;

        IDNS::SRVResultPtr mSRVResult;

        IDNS::SRVLookupTypes mLookupType;

        typedef std::list<IDNSQueryPtr> ResolverList;
        ResolverList mResolvers;
      };

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark DNSInstantResultQuery
      #pragma mark

      class DNSInstantResultQuery : public IDNSQuery
      {
      protected:
        DNSInstantResultQuery() : mID(zsLib::createPUID()) {}
      public:
        //---------------------------------------------------------------------
        static DNSInstantResultQueryPtr create() {return DNSInstantResultQueryPtr(new DNSInstantResultQuery);}

        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark DNSSRVResolverQuery => IDNSQuery
        #pragma mark

        //---------------------------------------------------------------------
        virtual PUID getID() const {return mID;}

        //---------------------------------------------------------------------
        virtual void cancel() {}

        //---------------------------------------------------------------------
        virtual bool hasResult() const {return mA || mAAAA || mSRV;}

        //---------------------------------------------------------------------
        virtual bool isComplete() const {return true;}

        //---------------------------------------------------------------------
        virtual AResultPtr getA() const {return IDNS::cloneA(mA);}

        //---------------------------------------------------------------------
        virtual AAAAResultPtr getAAAA() const {return IDNS::cloneAAAA(mAAAA);}

        //---------------------------------------------------------------------
        virtual SRVResultPtr getSRV() const {return IDNS::cloneSRV(mSRV);}

      public:
        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark DNSInstantResultQuery => (data)
        #pragma mark

        AResultPtr mA;
        AAAAResultPtr mAAAA;
        SRVResultPtr mSRV;

      private:
        PUID mID;
      };

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark DNSListQuery
      #pragma mark

      class DNSListQuery : public MessageQueueAssociator,
                           public IDNSQuery,
                           public IDNSDelegate
      {
      public:
        typedef IDNS::SRVLookupTypes SRVLookupTypes;
        typedef std::list<IDNSQueryPtr> DNSQueryList;

      protected:
        DNSListQuery(
                     IMessageQueuePtr queue,
                     IDNSDelegatePtr delegate
                     ) :
          MessageQueueAssociator(queue),
          mID(zsLib::createPUID()),
          mDelegate(IDNSDelegateProxy::createWeak(delegate))
        {
        }

        void init()
        {
        }

      public:
        //---------------------------------------------------------------------
        ~DNSListQuery()
        {
          mThisWeak.reset();
          cancel();
        }

        //---------------------------------------------------------------------
        static DNSListQueryPtr createSRV(
                                         IDNSDelegatePtr delegate,
                                         const StringList &dnsList,
                                         const char *service,
                                         const char *protocol,
                                         WORD defaultPort,
                                         WORD defaultPriority,
                                         WORD defaultWeight,
                                         SRVLookupTypes lookupType
                                         )
        {
          ZS_THROW_INVALID_USAGE_IF(!delegate)
          IMessageQueuePtr queue = Helper::getServiceQueue();
          ZS_THROW_BAD_STATE_IF(!queue)

          DNSListQueryPtr pThis(new DNSListQuery(queue, delegate));
          pThis->mThisWeak = pThis;

          for (StringList::const_iterator iter = dnsList.begin(); iter != dnsList.end(); ++iter)
          {
            const String &name = (*iter);
            IDNSQueryPtr query = IDNS::lookupSRV(pThis, name, service, protocol, defaultPort, defaultPriority, defaultWeight, lookupType);
            if (!query) {
              ZS_LOG_WARNING(Detail, pThis->log("lookupSRV returned NULL"))
              return DNSListQueryPtr();
            }
            pThis->mQueries.push_back(query);
          }

          pThis->init();
          return pThis;
        }

        //---------------------------------------------------------------------
        static DNSListQueryPtr createA(
                                       IDNSDelegatePtr delegate,
                                       const StringList &dnsList
                                       )
        {
          ZS_THROW_INVALID_USAGE_IF(!delegate)
          IMessageQueuePtr queue = Helper::getServiceQueue();
          ZS_THROW_BAD_STATE_IF(!queue)

          DNSListQueryPtr pThis(new DNSListQuery(queue, delegate));
          pThis->mThisWeak = pThis;

          for (StringList::const_iterator iter = dnsList.begin(); iter != dnsList.end(); ++iter)
          {
            const String &name = (*iter);
            IDNSQueryPtr query = IDNS::lookupA(pThis, name);
            if (!query) {
              ZS_LOG_WARNING(Detail, pThis->log("lookupA returned NULL"))
              return DNSListQueryPtr();
            }
            pThis->mQueries.push_back(query);
          }

          pThis->init();
          return pThis;
        }

        //---------------------------------------------------------------------
        static DNSListQueryPtr createAAAA(
                                          IDNSDelegatePtr delegate,
                                          const StringList &dnsList
                                          )
        {
          ZS_THROW_INVALID_USAGE_IF(!delegate)
          IMessageQueuePtr queue = Helper::getServiceQueue();
          ZS_THROW_BAD_STATE_IF(!queue)

          DNSListQueryPtr pThis(new DNSListQuery(queue, delegate));
          pThis->mThisWeak = pThis;

          for (StringList::const_iterator iter = dnsList.begin(); iter != dnsList.end(); ++iter)
          {
            const String &name = (*iter);
            IDNSQueryPtr query = IDNS::lookupAAAA(pThis, name);
            if (!query) {
              ZS_LOG_WARNING(Detail, pThis->log("lookupAAAA returned NULL"))
              return DNSListQueryPtr();
            }
            pThis->mQueries.push_back(query);
          }

          pThis->init();
          return pThis;
        }

        //---------------------------------------------------------------------
        static DNSListQueryPtr createAorAAAA(
                                             IDNSDelegatePtr delegate,
                                             const StringList &dnsList
                                             )
        {
          ZS_THROW_INVALID_USAGE_IF(!delegate)
          IMessageQueuePtr queue = Helper::getServiceQueue();
          ZS_THROW_BAD_STATE_IF(!queue)

          DNSListQueryPtr pThis(new DNSListQuery(queue, delegate));
          pThis->mThisWeak = pThis;

          for (StringList::const_iterator iter = dnsList.begin(); iter != dnsList.end(); ++iter)
          {
            const String &name = (*iter);
            IDNSQueryPtr query = IDNS::lookupAorAAAA(pThis, name);
            if (!query) {
              ZS_LOG_WARNING(Detail, pThis->log("lookupAorAAAA returned NULL"))
              return DNSListQueryPtr();
            }
            pThis->mQueries.push_back(query);
          }

          pThis->init();
          return pThis;
        }

        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark DNSListQuery => IDNSQuery
        #pragma mark

        //---------------------------------------------------------------------
        virtual PUID getID() const {return mID;}

        //---------------------------------------------------------------------
        virtual void cancel()
        {
          AutoRecursiveLock lock(mLock);
          ZS_LOG_DEBUG(log("cancel called"))

          for (DNSQueryList::iterator iter = mQueries.begin(); iter != mQueries.end(); ++iter)
          {
            IDNSQueryPtr &query = (*iter);
            ZS_LOG_DEBUG(log("cancelling DNS query") + ZS_PARAM("query ID", query->getID()))
            query->cancel();
          }
          mDelegate.reset();
        }

        //---------------------------------------------------------------------
        virtual bool hasResult() const
        {
          AutoRecursiveLock lock(mLock);
          return mA || mAAAA || mSRV;
        }

        //---------------------------------------------------------------------
        virtual bool isComplete() const
        {
          AutoRecursiveLock lock(mLock);
          return !mDelegate;
        }

        //---------------------------------------------------------------------
        virtual AResultPtr getA() const
        {
          AutoRecursiveLock lock(mLock);
          return IDNS::cloneA(mA);
        }

        //---------------------------------------------------------------------
        virtual AAAAResultPtr getAAAA() const
        {
          AutoRecursiveLock lock(mLock);
          return IDNS::cloneAAAA(mAAAA);
        }

        //---------------------------------------------------------------------
        virtual SRVResultPtr getSRV() const
        {
          AutoRecursiveLock lock(mLock);
          return IDNS::cloneSRV(mSRV);
        }

      protected:
        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark DNSListQuery => IDNSDelegate
        #pragma mark

        //---------------------------------------------------------------------
        virtual void onLookupCompleted(IDNSQueryPtr inQuery)
        {
          AutoRecursiveLock lock(mLock);
          ZS_LOG_DEBUG(log("query completed") + ZS_PARAM("query ID", inQuery->getID()))

          if (!mDelegate) {
            ZS_LOG_WARNING(Detail, log("query result came in after delegate was gone") + ZS_PARAM("query ID", inQuery->getID()))
            return;
          }

          for (DNSQueryList::iterator dnsIter = mQueries.begin(); dnsIter != mQueries.end(); )
          {
            DNSQueryList::iterator current = dnsIter;
            ++dnsIter;

            IDNSQueryPtr &query = (*current);
            if (query == inQuery) {
              ZS_LOG_DEBUG(log("found matching query thus removing query as it is done"))

              AResultPtr aResult = query->getA();
              if (aResult) {
                ZS_LOG_DEBUG(log("merging A result"))
                merge(mA, aResult);
              }

              AAAAResultPtr aaaaResult = query->getAAAA();
              if (aaaaResult) {
                ZS_LOG_DEBUG(log("merging AAAA result"))
                merge(mAAAA, aaaaResult);
              }

              SRVResultPtr srvResult = query->getSRV();
              if (srvResult) {
                ZS_LOG_DEBUG(log("merging SRV result"))
                merge(mSRV, srvResult);
              }

              mQueries.erase(current);
              break;
            }
          }

          if (mQueries.size() > 0) {
            ZS_LOG_DEBUG(log("waiting for more queries to complete") + ZS_PARAM("waiting total", mQueries.size()))
            return;
          }

          if (!hasResult()) {
            ZS_LOG_WARNING(Detail, log("all DNS queries in the list failed"))
          }

          sortSRV(mSRV);

          try {
            mDelegate->onLookupCompleted(mThisWeak.lock());
          } catch (IDNSDelegateProxy::Exceptions::DelegateGone &) {
            ZS_LOG_WARNING(Detail, log("delegate gone"))
            mDelegate.reset();
          }

          cancel();
        }

      private:
        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark DNSListQuery => (internal)
        #pragma mark

        //---------------------------------------------------------------------
        Log::Params log(const char *message) const
        {
          ElementPtr objectEl = Element::create("DNSListQuery");
          IHelper::debugAppend(objectEl, "id", mID);
          return Log::Params(message, objectEl);
        }

      private:
        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark DNSListQuery => (data)
        #pragma mark

        mutable RecursiveLock mLock;
        PUID mID;
        IDNSQueryWeakPtr mThisWeak;
        AResultPtr mA;
        AAAAResultPtr mAAAA;
        SRVResultPtr mSRV;

        IDNSDelegatePtr mDelegate;

        DNSQueryList mQueries;
      };


      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark DNS => IDNS
      #pragma mark

      //-----------------------------------------------------------------------
      IDNSQueryPtr DNS::lookupA(
                                IDNSDelegatePtr delegate,
                                const char *name
                                )
      {
        ZS_THROW_INVALID_USAGE_IF(!name)
        ZS_THROW_INVALID_USAGE_IF(String(name).length() < 1)

        IPAddressList ips;
        if (internal::isIPAddressList(name, 0, ips)) {
          internal::DNSInstantResultQueryPtr temp = internal::DNSInstantResultQuery::create();
          delegate = IDNSDelegateProxy::create(internal::Helper::getServiceQueue(), delegate);

          AResultPtr resultA = AResultPtr(new AResult);
          resultA->mName = name;
          resultA->mTTL = 3600;

          AAAAResultPtr resultAAAA = AAAAResultPtr(new AAAAResult);
          resultAAAA->mName = name;
          resultAAAA->mTTL = 3600;

          for (IPAddressList::iterator iter = ips.begin(); iter != ips.end(); ++iter) {
            const IPAddress &ip = (*iter);

            if (ip.isIPv4()) {
              ZS_LOG_DEBUG(log("A record found (no resolve required)") + ZS_PARAM("ip", ip.string()))
              temp->mA = resultA;
              resultA->mIPAddresses.push_back(ip);
            } else {
              ZS_LOG_ERROR(Debug, log("A record found ip but was IPv6 address for A record lookup") + ZS_PARAM("input", name) + ZS_PARAM("result ip", ip.string()))
              temp->mAAAA = resultAAAA;
              resultAAAA->mIPAddresses.push_back(ip);
            }
          }
          delegate->onLookupCompleted(temp);
          return temp;
        }

        ZS_LOG_DEBUG(log("A lookup") + ZS_PARAM("name", name))

        StringList dnsList;
        if (internal::isDNSsList(name, dnsList)) {
          return internal::DNSListQuery::createA(delegate, dnsList);
        }

        return internal::DNSAQuery::create(delegate, name);
      }

      //-----------------------------------------------------------------------
      IDNSQueryPtr DNS::lookupAAAA(
                                   IDNSDelegatePtr delegate,
                                   const char *name
                                   )
      {
        ZS_THROW_INVALID_USAGE_IF(!name)
        ZS_THROW_INVALID_USAGE_IF(String(name).length() < 1)

        IPAddressList ips;
        if (internal::isIPAddressList(name, 0, ips)) {
          internal::DNSInstantResultQueryPtr temp = internal::DNSInstantResultQuery::create();
          delegate = IDNSDelegateProxy::create(internal::Helper::getServiceQueue(), delegate);

          AResultPtr result = AResultPtr(new AResult);
          result->mName = name;
          result->mTTL = 3600;
          result->mIPAddresses = ips;

          temp->mAAAA = result;

          for (IPAddressList::iterator iter = ips.begin(); iter != ips.end(); ++iter) {
            const IPAddress &ip = (*iter);
            ZS_LOG_DEBUG(log("AAAA record found (no resolve required") + ZS_PARAM("ip", ip.string()))
          }

          delegate->onLookupCompleted(temp);
          return temp;
        }

        ZS_LOG_DEBUG(log("AAAA lookup") + ZS_PARAM("name", name))

        StringList dnsList;
        if (internal::isDNSsList(name, dnsList)) {
          return internal::DNSListQuery::createAAAA(delegate, dnsList);
        }

        return internal::DNSAAAAQuery::create(delegate, name);
      }

      //-----------------------------------------------------------------------
      IDNSQueryPtr DNS::lookupAorAAAA(
                                      IDNSDelegatePtr delegate,
                                      const char *name
                                      )
      {
        ZS_THROW_INVALID_USAGE_IF(!name)
        ZS_THROW_INVALID_USAGE_IF(String(name).length() < 1)

        IPAddressList ips;
        if (internal::isIPAddressList(name, 0, ips)) {
          internal::DNSInstantResultQueryPtr temp = internal::DNSInstantResultQuery::create();
          delegate = IDNSDelegateProxy::create(internal::Helper::getServiceQueue(), delegate);

          AResultPtr resultA = AResultPtr(new AResult);
          resultA->mName = name;
          resultA->mTTL = 3600;

          AAAAResultPtr resultAAAA = AAAAResultPtr(new AAAAResult);
          resultAAAA->mName = name;
          resultAAAA->mTTL = 3600;

          for (IPAddressList::iterator iter = ips.begin(); iter != ips.end(); ++iter) {
            const IPAddress &ip = (*iter);

            if (ip.isIPv4()) {
              ZS_LOG_DEBUG(log("A or AAAA record found A record (no resolve required)") + ZS_PARAM("input", name) + ZS_PARAM("result ip", ip.string()))
              temp->mA = resultA;
              resultA->mIPAddresses.push_back(ip);
            } else {
              ZS_LOG_DEBUG(log("A or AAAA record found AAAA record (no resolve required)") + ZS_PARAM("input", name) + ZS_PARAM("result ip", ip.string()))
              temp->mAAAA = resultAAAA;
              resultAAAA->mIPAddresses.push_back(ip);
            }
          }
          delegate->onLookupCompleted(temp);
          return temp;
        }

        ZS_LOG_DEBUG(log("A or AAAA lookup") + ZS_PARAM("name", name))

        StringList dnsList;
        if (internal::isDNSsList(name, dnsList)) {
          return internal::DNSListQuery::createAorAAAA(delegate, dnsList);
        }

        return internal::DNSAorAAAAQuery::create(delegate, name);
      }

      //-----------------------------------------------------------------------
      IDNSQueryPtr DNS::lookupSRV(
                                  IDNSDelegatePtr delegate,
                                  const char *name,
                                  const char *service,
                                  const char *protocol,
                                  WORD defaultPort,
                                  WORD defaultPriority,
                                  WORD defaultWeight,
                                  SRVLookupTypes lookupType
                                  )
      {
        ZS_THROW_INVALID_USAGE_IF(!delegate)
        ZS_THROW_INVALID_USAGE_IF(!name)
        ZS_THROW_INVALID_USAGE_IF(String(name).length() < 1)

        IPAddressList ips;
        if (internal::isIPAddressList(name, defaultPort, ips)) {
          internal::DNSInstantResultQueryPtr temp = internal::DNSInstantResultQuery::create();
          delegate = IDNSDelegateProxy::create(internal::Helper::getServiceQueue(), delegate);

          SRVResultPtr result(new SRVResult);
          result->mName = name;
          result->mService = service;
          result->mProtocol = protocol;
          result->mTTL = 3600;

          SRVResult::SRVRecord record;
          record.mPriority = defaultPriority;
          record.mWeight = defaultWeight;
          record.mPort = defaultPort;
          record.mName = name;

          AResultPtr resultA = AResultPtr(new AResult);
          resultA->mName = name;
          resultA->mTTL = 3600;

          AAAAResultPtr resultAAAA = AAAAResultPtr(new AAAAResult);
          resultAAAA->mName = name;
          resultAAAA->mTTL = 3600;

          for (IPAddressList::iterator iter = ips.begin(); iter != ips.end(); ++iter) {
            const IPAddress &ip = (*iter);

            if (ip.isIPv4()) {
              resultA->mIPAddresses.push_back(ip);
              record.mAResult = resultA;
            } else {
              resultAAAA->mIPAddresses.push_back(ip);
              record.mAAAAResult = resultAAAA;
            }

            ZS_LOG_DEBUG(log("SRV record found SRV record (no resolve required") + ZS_PARAM("input", name) + ZS_PARAM("result ip", ip.string()))
          }

          result->mRecords.push_back(record);

          internal::sortSRV(result);

          temp->mSRV = result;
          delegate->onLookupCompleted(temp);
          return temp;
        }

        ZS_LOG_DEBUG(log("SRV lookup") + ZS_PARAM("name", name) + ZS_PARAM("service", service) + ZS_PARAM("protocol", protocol) + ZS_PARAM("default port", defaultPort) + ZS_PARAM("type", (int)lookupType))
        
        StringList dnsList;
        if (internal::isDNSsList(name, dnsList)) {
          return internal::DNSListQuery::createSRV(delegate, dnsList, service, protocol, defaultPort, defaultPriority, defaultWeight, lookupType);
        }
        
        if (SRVLookupType_LookupOnly != lookupType) {
          return internal::DNSSRVResolverQuery::create(delegate, name, service, protocol, defaultPort, defaultPriority, defaultWeight, lookupType);
        }
        return internal::DNSSRVQuery::create(delegate, name, service, protocol);
      }
      
      //-----------------------------------------------------------------------
      Log::Params DNS::log(const char *message)
      {
        return Log::Params(message, "DNS");
      }

    }

    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    #pragma mark
    #pragma mark IDNS
    #pragma mark

    //-------------------------------------------------------------------------
    IDNSQueryPtr IDNS::lookupA(
                               IDNSDelegatePtr delegate,
                               const char *name
                               )
    {
      return internal::IDNSFactory::singleton().lookupA(delegate, name);
    }

    //-------------------------------------------------------------------------
    IDNSQueryPtr IDNS::lookupAAAA(
                                  IDNSDelegatePtr delegate,
                                  const char *name
                                  )
    {
      return internal::IDNSFactory::singleton().lookupAAAA(delegate, name);
    }

    //-------------------------------------------------------------------------
    IDNSQueryPtr IDNS::lookupAorAAAA(
                                     IDNSDelegatePtr delegate,
                                     const char *name
                                     )
    {
      return internal::IDNSFactory::singleton().lookupAorAAAA(delegate, name);
    }

    //-------------------------------------------------------------------------
    IDNSQueryPtr IDNS::lookupSRV(
                                 IDNSDelegatePtr delegate,
                                 const char *name,
                                 const char *service,
                                 const char *protocol,
                                 WORD defaultPort,
                                 WORD defaultPriority,
                                 WORD defaultWeight,
                                 SRVLookupTypes lookupType
                                 )
    {
      return internal::IDNSFactory::singleton().lookupSRV(delegate, name, service, protocol, defaultPort, defaultPriority, defaultWeight, lookupType);
    }
    
    //-------------------------------------------------------------------------
    IDNS::AResultPtr IDNS::convertIPAddressesToAResult(
                                                       const std::list<IPAddress> &ipAddresses,
                                                       UINT ttl
                                                       )
    {
      AResultPtr result(new AResult);

      result->mTTL = ttl;
      internal::copyToAddressList(ipAddresses, result->mIPAddresses, true, false);
      if (result->mIPAddresses.size() < 1) return IDNS::AResultPtr();

      result->mName = result->mIPAddresses.front().string(false);
      return result;
    }

    //-------------------------------------------------------------------------
    IDNS::AAAAResultPtr IDNS::convertIPAddressesToAAAAResult(
                                                             const std::list<IPAddress> &ipAddresses,
                                                             UINT ttl
                                                             )
    {
      AAAAResultPtr result(new AAAAResult);

      result->mTTL = ttl;
      internal::copyToAddressList(ipAddresses, result->mIPAddresses, false, true);
      if (result->mIPAddresses.size() < 1) return IDNS::AAAAResultPtr();

      result->mName = result->mIPAddresses.front().string(false);
      return result;
    }

    //-------------------------------------------------------------------------
    IDNS::SRVResultPtr IDNS::convertAorAAAAResultToSRVResult(
                                                             const char *service,
                                                             const char *protocol,
                                                             AResultPtr resultA,
                                                             AAAAResultPtr resultAAAA,
                                                             WORD defaultPort,
                                                             WORD defaultPriority,
                                                             WORD defaultWeight
                                                             )
    {
      ZS_THROW_INVALID_USAGE_IF((!resultA) && (!resultAAAA))

      IDNS::AResultPtr useResult = (resultA ? resultA : resultAAAA);

      SRVResultPtr result(new SRVResult);
      result->mName = useResult->mName;
      result->mService = service;
      result->mProtocol = protocol;
      result->mTTL = useResult->mTTL;

      // just in case the result AAAA's TTL is lower then the resultA's TTL set the SRV result's TTL to the lower of the two values
      if (resultAAAA) {
        if (resultAAAA->mTTL < result->mTTL)
          result->mTTL = resultAAAA->mTTL;
      }

      if (resultA) {
        SRVResult::SRVRecord record;

        AResultPtr aResult(new AResult);
        aResult->mName = resultA->mName;
        aResult->mTTL = resultA->mTTL;
        internal::copyToAddressList(resultA->mIPAddresses, aResult->mIPAddresses);
        internal::fixDefaultPort(aResult, defaultPort);

        record.mName = resultA->mName;
        record.mPriority = defaultPriority;
        record.mWeight = defaultWeight;
        if (aResult->mIPAddresses.size() > 0)
          record.mPort = aResult->mIPAddresses.front().getPort();
        else
          record.mPort = 0;
        record.mAResult = aResult;
        result->mRecords.push_back(record);
      }
      if (resultAAAA) {
        SRVResult::SRVRecord record;

        AAAAResultPtr aaaaResult(new AAAAResult);
        aaaaResult->mName = resultAAAA->mName;
        aaaaResult->mTTL = resultAAAA->mTTL;
        internal::copyToAddressList(resultAAAA->mIPAddresses, aaaaResult->mIPAddresses);
        internal::fixDefaultPort(aaaaResult, defaultPort);

        record.mName = resultAAAA->mName;
        record.mPriority = defaultPriority;
        record.mWeight = defaultWeight;
        if (aaaaResult->mIPAddresses.size() > 0)
          record.mPort = aaaaResult->mIPAddresses.front().getPort();
        else
          record.mPort = 0;
        record.mAAAAResult = aaaaResult;
        result->mRecords.push_back(record);
      }
      return result;
    }

    //-------------------------------------------------------------------------
    IDNS::SRVResultPtr IDNS::convertIPAddressesToSRVResult(
                                                           const char *service,
                                                           const char *protocol,
                                                           const std::list<IPAddress> &ipAddresses,
                                                           WORD defaultPort,
                                                           WORD defaultPriority,
                                                           WORD defaultWeight,
                                                           UINT ttl
                                                           )
    {
      ZS_THROW_INVALID_USAGE_IF(ipAddresses.size() < 1)

      AResultPtr aResult = IDNS::convertIPAddressesToAResult(ipAddresses, ttl);
      AAAAResultPtr aaaaResult = IDNS::convertIPAddressesToAAAAResult(ipAddresses, ttl);

      ZS_THROW_BAD_STATE_IF((!aResult) && (!aaaaResult))  // how can this happen??

      return IDNS::convertAorAAAAResultToSRVResult(
                                                   service,
                                                   protocol,
                                                   aResult,
                                                   aaaaResult,
                                                   defaultPort,
                                                   defaultPriority,
                                                   defaultWeight
                                                   );
    }

    //-------------------------------------------------------------------------
    IDNS::SRVResultPtr IDNS::mergeSRVs(const SRVResultList &srvList)
    {
      if (srvList.size() < 1) return SRVResultPtr();

      SRVResultPtr finalSRV;
      for (SRVResultList::const_iterator iter = srvList.begin(); iter != srvList.end(); ++iter)
      {
        const SRVResultPtr &result = (*iter);
        if (!finalSRV) {
          finalSRV = IDNS::cloneSRV(result);
        } else {
          internal::merge(finalSRV, result);
        }
      }

      if (srvList.size() > 1) {
        internal::sortSRV(finalSRV);
      }

      return finalSRV;
    }

    //-------------------------------------------------------------------------
    bool IDNS::extractNextIP(
                             SRVResultPtr srvResult,
                             IPAddress &outIP,
                             AResultPtr *outAResult,
                             AAAAResultPtr *outAAAAResult
                             )
    {
      if (outAResult)
        *outAResult = AResultPtr();
      if (outAAAAResult)
        *outAAAAResult = AAAAResultPtr();

      outIP.clear();

      if (!srvResult) return false;

      while (true)
      {
        if (srvResult->mRecords.size() < 1) {
          ZS_LOG_DEBUG(Log::Params("DNS found no IPs to extract (i.e. end of list).", "IDNS"))
          return false;
        }

        SRVResult::SRVRecord &record = srvResult->mRecords.front();
        if ((!record.mAResult) && (!record.mAAAAResult)) {
          srvResult->mRecords.pop_front();
          continue; // try again
        }

        AResultPtr &useResult = (record.mAResult ? record.mAResult : record.mAAAAResult);
        if (useResult->mIPAddresses.size() < 1) {
          useResult.reset();
          continue;
        }

        outIP = useResult->mIPAddresses.front();
        useResult->mIPAddresses.pop_front();

        ZS_LOG_DEBUG(Log::Params("DNS extracted next IP", "IDNS") + ZS_PARAM("ip", outIP.string()))

        // give caller indication of which list it came from
        if (outAResult) {
          if (record.mAResult)
            *outAResult = record.mAResult;
        }
        if (outAAAAResult) {
          if (!record.mAResult)
            *outAAAAResult = record.mAAAAResult;
        }
        return true;
      }

      return false;
    }

    //-------------------------------------------------------------------------
    // PURPOSE: Clone routines for various return results.
    IDNS::AResultPtr IDNS::cloneA(AResultPtr result)
    {
      if (!result) return result;

      AResultPtr clone(new AResult);
      clone->mName = result->mName;
      clone->mTTL = result->mTTL;
      internal::copyToAddressList(result->mIPAddresses, clone->mIPAddresses);
      return clone;
    }

    //-------------------------------------------------------------------------
    IDNS::AAAAResultPtr IDNS::cloneAAAA(AAAAResultPtr result)
    {
      return cloneA(result);
    }

    //-------------------------------------------------------------------------
    IDNS::SRVResultPtr IDNS::cloneSRV(SRVResultPtr srvResult)
    {
      if (!srvResult) return srvResult;

      SRVResultPtr clone(new SRVResult);
      clone->mName = srvResult->mName;
      clone->mService = srvResult->mService;
      clone->mProtocol = srvResult->mProtocol;
      clone->mTTL = srvResult->mTTL;

      for (SRVResult::SRVRecordList::const_iterator iter = srvResult->mRecords.begin(); iter != srvResult->mRecords.end(); ++iter) {
        SRVResult::SRVRecord record;
        record.mName = (*iter).mName;
        record.mPriority = (*iter).mPriority;
        record.mWeight = (*iter).mWeight;
        record.mPort = (*iter).mPort;
        record.mAResult = cloneA((*iter).mAResult);
        record.mAAAAResult = cloneAAAA((*iter).mAAAAResult);
        clone->mRecords.push_back(record);
      }

      return clone;
    }
  }
}
