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

#include "boost_replacement.h"
#include "config.h"

#include <zsLib/types.h>
#include <zsLib/helpers.h>
#include <zsLib/Log.h>
#include <openpeer/services/ILogger.h>

#include <iostream>

namespace openpeer { namespace services { namespace test { ZS_IMPLEMENT_SUBSYSTEM(openpeer_services_test) } } }



typedef openpeer::services::ILogger ILogger;

void doTestCanonicalXML();
void doTestDH();
void doTestDNS();
void doTestICESocket();
void doTestSTUNDiscovery();
void doTestTURNSocket();
void doTestRUDPListener();
void doTestRUDPICESocket();
void doTestRUDPICESocketLoopback();
void doTestTCPMessagingLoopback();

namespace BoostReplacement
{
  zsLib::ULONG &getGlobalPassedVar()
  {
    static zsLib::ULONG value = 0;
    return value;
  }
  
  zsLib::ULONG &getGlobalFailedVar()
  {
    static zsLib::ULONG value = 0;
    return value;
  }
  
  void passed()
  {
    zsLib::atomicIncrement(getGlobalPassedVar());
  }
  void failed()
  {
    zsLib::atomicIncrement(getGlobalFailedVar());
  }
  
  void installLogger()
  {
    BOOST_STDOUT() << "INSTALLING LOGGER...\n\n";
    ILogger::setLogLevel(zsLib::Log::Trace);
    ILogger::setLogLevel("zsLib", zsLib::Log::Trace);
    ILogger::setLogLevel("openpeer_services", zsLib::Log::Trace);
    ILogger::setLogLevel("openpeer_services_http", zsLib::Log::Trace);

    if (OPENPEER_SERVICE_TEST_USE_STDOUT_LOGGING) {
      ILogger::installStdOutLogger(false);
    }

    if (OPENPEER_SERVICE_TEST_USE_FIFO_LOGGING) {
      ILogger::installFileLogger(OPENPEER_SERVICE_TEST_FIFO_LOGGING_FILE, true);
    }

    if (OPENPEER_SERVICE_TEST_USE_TELNET_LOGGING) {
      bool serverMode = (OPENPEER_SERVICE_TEST_DO_RUDPICESOCKET_CLIENT_TO_SERVER_TEST) && (!OPENPEER_SERVICE_TEST_RUNNING_AS_CLIENT);
      ILogger::installTelnetLogger(serverMode ? OPENPEER_SERVICE_TEST_TELNET_SERVER_LOGGING_PORT : OPENPEER_SERVICE_TEST_TELNET_LOGGING_PORT, 60, true);

      for (int tries = 0; tries < 60; ++tries)
      {
        if (ILogger::isTelnetLoggerListening()) {
          break;
        }
        boost::this_thread::sleep(zsLib::Seconds(1));
      }
    }

    if (OPENPEER_SERVICE_TEST_USE_DEBUGGER_LOGGING) {
      ILogger::installDebuggerLogger();
    }

    BOOST_STDOUT() << "INSTALLED LOGGER...\n\n";
  }
  
  void uninstallLogger()
  {
    BOOST_STDOUT() << "REMOVING LOGGER...\n\n";

    if (OPENPEER_SERVICE_TEST_USE_STDOUT_LOGGING) {
      ILogger::uninstallStdOutLogger();
    }
    if (OPENPEER_SERVICE_TEST_USE_FIFO_LOGGING) {
      ILogger::uninstallFileLogger();
    }
    if (OPENPEER_SERVICE_TEST_USE_TELNET_LOGGING) {
      ILogger::uninstallTelnetLogger();
    }
    if (OPENPEER_SERVICE_TEST_USE_DEBUGGER_LOGGING) {
      ILogger::uninstallDebuggerLogger();
    }

    BOOST_STDOUT() << "REMOVED LOGGER...\n\n";
  }
  
  void output()
  {
    BOOST_STDOUT() << "PASSED:       [" << BoostReplacement::getGlobalPassedVar() << "]\n";
    if (0 != BoostReplacement::getGlobalFailedVar()) {
      BOOST_STDOUT() << "***FAILED***: [" << BoostReplacement::getGlobalFailedVar() << "]\n";
    }
  }
  
  void runAllTests()
  {
    BOOST_INSTALL_LOGGER()

    BOOST_RUN_TEST_FUNC(doTestCanonicalXML)
    BOOST_RUN_TEST_FUNC(doTestDH)
    BOOST_RUN_TEST_FUNC(doTestDNS)
    BOOST_RUN_TEST_FUNC(doTestICESocket)
    BOOST_RUN_TEST_FUNC(doTestSTUNDiscovery)
    BOOST_RUN_TEST_FUNC(doTestTURNSocket)
    BOOST_RUN_TEST_FUNC(doTestRUDPICESocketLoopback)
    BOOST_RUN_TEST_FUNC(doTestRUDPListener)
    BOOST_RUN_TEST_FUNC(doTestRUDPICESocket)
    BOOST_RUN_TEST_FUNC(doTestTCPMessagingLoopback)

    BOOST_UNINSTALL_LOGGER()
  }
}
