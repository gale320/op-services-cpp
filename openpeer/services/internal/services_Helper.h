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
#include <openpeer/services/IHelper.h>

#include <zsLib/String.h>

#define OPENPEER_SERVICES_SETTING_HELPER_SERVICES_THREAD_PRIORITY       "openpeer/services/services-thread-priority"
#define OPENPEER_SERVICES_SETTING_HELPER_LOGGER_THREAD_PRIORITY         "openpeer/services/logger-thread-priority"
#define OPENPEER_SERVICES_SETTING_HELPER_SOCKET_MONITOR_THREAD_PRIORITY "openpeer/services/socket-monitor-thread-priority"
#define OPENPEER_SERVICES_SETTING_HELPER_TIMER_MONITOR_THREAD_PRIORITY  "openpeer/services/timer-monitor-thread-priority"

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
      #pragma mark Helper
      #pragma mark

      class Helper : public IHelper
      {
      public:
        static void debugAppend(ElementPtr &parentEl, const char *name, const char *value);
        static void debugAppend(ElementPtr &parentEl, const char *name, const String &value);
        static void debugAppendNumber(ElementPtr &parentEl, const char *name, const String &value);
        static void debugAppend(ElementPtr &parentEl, const char *name, bool value, bool ignoreIfFalse = true);
        static void debugAppend(ElementPtr &parentEl, const Log::Param &param);
        static void debugAppend(ElementPtr &parentEl, const char *name, ElementPtr childEl);
        static void debugAppend(ElementPtr &parentEl, ElementPtr childEl);

        static String toString(ElementPtr el);
        static ElementPtr toJSON(const char *str);

        static String timeToString(const Time &value);
        static Time stringToTime(const String &str);

        static String randomString(UINT lengthInChars);
        static ULONG random(ULONG minValue, ULONG maxValue);

        static SecureByteBlockPtr random(size_t lengthInBytes);

        static int compare(
                           const SecureByteBlock &left,
                           const SecureByteBlock &right
                           );

        static bool isEmpty(SecureByteBlockPtr buffer);
        static bool isEmpty(const SecureByteBlock &buffer);

        static bool hasData(SecureByteBlockPtr buffer);
        static bool hasData(const SecureByteBlock &buffer);

        static SecureByteBlockPtr clone(SecureByteBlockPtr pBuffer);
        static SecureByteBlockPtr clone(const SecureByteBlock &buffer);

        static String convertToString(const SecureByteBlock &buffer);
        static SecureByteBlockPtr convertToBuffer(const char *input);
        static SecureByteBlockPtr convertToBuffer(
                                                  const BYTE *buffer,
                                                  size_t bufferLengthInBytes
                                                  );
        static SecureByteBlockPtr convertToBuffer(
                                                  boost::shared_array<char> arrayStr,
                                                  size_t lengthInChars = SIZE_T_MAX,
                                                  bool wipeOriginal = true
                                                  );

        static String convertToBase64(
                                      const BYTE *buffer,
                                      size_t bufferLengthInBytes
                                      );

        static String convertToBase64(const SecureByteBlock &input);

        static String convertToBase64(const String &input);

        static SecureByteBlockPtr convertFromBase64(const String &input);

        static String convertToHex(
                                   const BYTE *buffer,
                                   size_t bufferLengthInBytes,
                                   bool outputUpperCase = false
                                   );

        static String convertToHex(
                                   const SecureByteBlock &input,
                                   bool outputUpperCase = false
                                   );

        static SecureByteBlockPtr convertFromHex(const String &input);

        static SecureByteBlockPtr encrypt(
                                          const SecureByteBlock &key, // key length of 32 = AES/256
                                          const SecureByteBlock &iv,
                                          const SecureByteBlock &buffer,
                                          EncryptionAlgorthms algorithm = EncryptionAlgorthm_AES
                                          );

        static SecureByteBlockPtr encrypt(
                                          const SecureByteBlock &key, // key length of 32 = AES/256
                                          const SecureByteBlock &iv,
                                          const char *value,
                                          EncryptionAlgorthms algorithm = EncryptionAlgorthm_AES
                                          );

        static SecureByteBlockPtr encrypt(
                                          const SecureByteBlock &key, // key length of 32 = AES/256
                                          const SecureByteBlock &iv,
                                          const BYTE *buffer,
                                          size_t bufferLengthInBytes,
                                          EncryptionAlgorthms algorithm = EncryptionAlgorthm_AES
                                          );

        static SecureByteBlockPtr decrypt(
                                          const SecureByteBlock &key,
                                          const SecureByteBlock &iv,
                                          const SecureByteBlock &buffer,
                                          EncryptionAlgorthms algorithm = EncryptionAlgorthm_AES
                                          );

        static size_t getHashDigestSize(HashAlgorthms algorithm);

        static SecureByteBlockPtr hash(
                                       const char *buffer,
                                       HashAlgorthms algorithm = HashAlgorthm_SHA1
                                       );

        static SecureByteBlockPtr hash(
                                       const SecureByteBlock &buffer,
                                       HashAlgorthms algorithm = HashAlgorthm_SHA1
                                       );

        static SecureByteBlockPtr hmacKeyFromPassphrase(const char *passphrase);
        static SecureByteBlockPtr hmacKeyFromPassphrase(const std::string &passphrase);

        static SecureByteBlockPtr hmac(
                                       const SecureByteBlock &key,
                                       const String &value,
                                       HashAlgorthms algorithm = HashAlgorthm_SHA1
                                       );

        static SecureByteBlockPtr hmac(
                                       const SecureByteBlock &key,
                                       const SecureByteBlock &buffer,
                                       HashAlgorthms algorithm = HashAlgorthm_SHA1
                                       );

        static SecureByteBlockPtr hmac(
                                       const SecureByteBlock &key,
                                       const BYTE *buffer,
                                       size_t bufferLengthInBytes,
                                       HashAlgorthms algorithm = HashAlgorthm_SHA1
                                       );

        static void splitKey(
                             const SecureByteBlock &key,
                             SecureByteBlockPtr &part1,
                             SecureByteBlockPtr &part2
                             );
        static SecureByteBlockPtr combineKey(
                                             const SecureByteBlockPtr &part1,
                                             const SecureByteBlockPtr &part2
                                             );

        static ElementPtr getSignatureInfo(
                                           ElementPtr signedEl,
                                           ElementPtr *outSignatureEl = NULL,
                                           String *outFullPublicKey = NULL,
                                           String *outFingerprint = NULL
                                           );

        static ElementPtr cloneAsCanonicalJSON(ElementPtr element);

        static bool isValidDomain(const char *domain);

        static void split(
                          const String &input,
                          SplitMap &outResult,
                          char splitChar
                          );
        
        static const String &get(
                                 const SplitMap &inResult,
                                 Index index
                                 );

        static String getDebugString(
                                     const BYTE *buffer,
                                     size_t bufferSizeInBytes,
                                     ULONG bytesPerGroup = 4,
                                     ULONG maxLineLength = 160
                                     );

        static Log::Params log(const char *message);

        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark (other)
        #pragma mark

        typedef std::map<IPAddress, bool> IPAddressMap;

        static void parseIPs(
                             const String &ipList,
                             IPAddressMap &outMap
                             );
        static bool containsIP(
                               const IPAddressMap &inMap,
                               const IPAddress &ip,
                               bool emptyMapReturns = true
                               );
      };
    }
  }
}
