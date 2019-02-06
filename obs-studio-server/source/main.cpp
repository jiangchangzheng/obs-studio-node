// Server program for the OBS Studio node module.
// Copyright(C) 2017 Streamlabs (General Workings Inc)
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110 - 1301, USA.

#include <chrono>
#include <inttypes.h>
#include <iostream>
#include <ipc-class.hpp>
#include <ipc-function.hpp>
#include <ipc-server.hpp>
#include <memory>
#include <thread>
#include <vector>
#include "error.hpp"
#include "nodeobs_api.h"
#include "nodeobs_autoconfig.h"
#include "nodeobs_content.h"
#include "nodeobs_service.h"
#include "nodeobs_settings.h"
#include "osn-fader.hpp"
#include "osn-filter.hpp"
#include "osn-global.hpp"
#include "osn-input.hpp"
#include "osn-module.hpp"
#include "osn-properties.hpp"
#include "osn-scene.hpp"
#include "osn-sceneitem.hpp"
#include "osn-source.hpp"
#include "osn-transition.hpp"
#include "osn-video.hpp"
#include "osn-volmeter.hpp"

#ifndef _DEBUG
#include "client/crash_report_database.h"
#include "client/crashpad_client.h"
#include "client/settings.h"
#endif

#if defined(_WIN32)
#include "Shlobj.h"

// Checks ForceGPUAsRenderDevice setting
extern "C" __declspec(dllexport) DWORD NvOptimusEnablement = [] {
	LPWSTR       roamingPath;
	std::wstring filePath;
	std::string  line;
	std::fstream file;
	bool         settingValue = true; // Default value (NvOptimusEnablement = 1)

	if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roamingPath))) {
		// Couldn't find roaming app data folder path, assume default value
		return settingValue;
	} else {
		filePath.assign(roamingPath);
		filePath.append(L"\\slobs-client\\basic.ini");
		CoTaskMemFree(roamingPath);
	}

	file.open(filePath);

	if (file.is_open()) {
		while (std::getline(file, line)) {
			if (line.find("ForceGPUAsRenderDevice", 0) != std::string::npos) {
				if (line.substr(line.find('=') + 1) == "false") {
					settingValue = false;
					file.close();
					break;
				}
			}
		}
	} else {
		//Couldn't open config file, assume default value
		return settingValue;
	}

	// Return setting value
	return settingValue;
}();
#endif

#define BUFFSIZE 512

struct ServerData
{
	std::mutex                                     mtx;
	std::chrono::high_resolution_clock::time_point last_connect, last_disconnect;
	size_t                                         count_connected = 0;
};

bool ServerConnectHandler(void* data, int64_t)
{
	ServerData*                  sd = reinterpret_cast<ServerData*>(data);
	std::unique_lock<std::mutex> ulock(sd->mtx);
	sd->last_connect = std::chrono::high_resolution_clock::now();
	sd->count_connected++;
	return true;
}

void ServerDisconnectHandler(void* data, int64_t)
{
	ServerData*                  sd = reinterpret_cast<ServerData*>(data);
	std::unique_lock<std::mutex> ulock(sd->mtx);
	sd->last_disconnect = std::chrono::high_resolution_clock::now();
	sd->count_connected--;
}

namespace System
{
	static void
	    Shutdown(void* data, const int64_t id, const std::vector<ipc::value>& args, std::vector<ipc::value>& rval)
	{
		bool* shutdown = (bool*)data;
		*shutdown      = true;
		rval.push_back(ipc::value((uint64_t)ErrorCode::Ok));
		return;
	}
} // namespace System

std::string FormatVAString(const char* const format, va_list args)
{
	auto         temp   = std::vector<char>{};
	auto         length = std::size_t{63};
	while (temp.size() <= length) {
		temp.resize(length + 1);
		const auto status = std::vsnprintf(temp.data(), temp.size(), format, args);
		if (status < 0)
			throw std::runtime_error{"string formatting error"};
		length = static_cast<std::size_t>(status);
	}
	return std::string{temp.data(), length};
}

int main(int argc, char* argv[])
{
#ifndef _DEBUG

	struct CrashpadInfo
	{
		base::FilePath&           handler;
		base::FilePath&           db;
		std::string&              url;
		std::vector<std::string>& arguments;
		crashpad::CrashpadClient& client;
	};

	std::wstring             appdata_path;
	crashpad::CrashpadClient client;
	bool                     rc;

#if defined(_WIN32)
	HRESULT hResult;
	PWSTR   ppszPath;

	hResult = SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, NULL, &ppszPath);

	appdata_path.assign(ppszPath);
	appdata_path.append(L"\\obs-studio-node-server");

	CoTaskMemFree(ppszPath);
#endif

	std::map<std::string, std::string> annotations;
	std::vector<std::string>           arguments;
	arguments.push_back("--no-rate-limit");

	std::wstring handler_path(L"crashpad_handler.exe");
	std::string  url(
        "https://sentry.io/api/1283431/minidump/?sentry_key=ec98eac4e3ce49c7be1d83c8fb2005ef");

	base::FilePath db(appdata_path);
	base::FilePath handler(handler_path);

	std::unique_ptr<crashpad::CrashReportDatabase> database = crashpad::CrashReportDatabase::Initialize(db);
	if (database == nullptr || database->GetSettings() == NULL)
		return false;

	database->GetSettings()->SetUploadsEnabled(true);

	rc = client.StartHandler(handler, db, db, url, annotations, arguments, true, true);
	/* TODO Check rc value for errors */

	rc = client.WaitForHandlerStart(INFINITE);
	/* TODO Check rc value for errors */

	// Setup the crashpad info that will be used in case the obs throws an error
	CrashpadInfo crashpadInfo = {handler, db, url, arguments, client};

#endif

	// Usage:
	// argv[0] = Path to this application. (Usually given by default if run via path-based command!)
	// argv[1] = Path to a named socket.

	if (argc != 2) {
		std::cerr << "There must be exactly one parameter." << std::endl;
		return -1;
	}

	// Instance
	ipc::server myServer;
	bool        doShutdown = false;
	ServerData  sd;
	sd.last_disconnect = sd.last_connect = std::chrono::high_resolution_clock::now();
	sd.count_connected                   = 0;

	// Classes
	/// System
	{
		std::shared_ptr<ipc::collection> cls = std::make_shared<ipc::collection>("System");
		cls->register_function(
		    std::make_shared<ipc::function>("Shutdown", std::vector<ipc::type>{}, System::Shutdown, &doShutdown));
		myServer.register_collection(cls);
	};

	/// OBS Studio Node
	osn::Global::Register(myServer);
	osn::Source::Register(myServer);
	osn::Input::Register(myServer);
	osn::Filter::Register(myServer);
	osn::Transition::Register(myServer);
	osn::Scene::Register(myServer);
	osn::SceneItem::Register(myServer);
	osn::Fader::Register(myServer);
	osn::VolMeter::Register(myServer);
	osn::Properties::Register(myServer);
	osn::Video::Register(myServer);
	osn::Module::Register(myServer);
	OBS_API::Register(myServer);
	OBS_content::Register(myServer);
	OBS_service::Register(myServer);
	OBS_settings::Register(myServer);
	OBS_settings::Register(myServer);
	autoConfig::Register(myServer);

	// Register Connect/Disconnect Handlers
	myServer.set_connect_handler(ServerConnectHandler, &sd);
	myServer.set_disconnect_handler(ServerDisconnectHandler, &sd);

#ifndef _DEBUG

	// Handler for obs errors (mainly for bcrash() calls)
	base_set_crash_handler([](const char* format, va_list args, void* param) { 

		CrashpadInfo* crashpadInfo = (CrashpadInfo*)param;
		std::map<std::string, std::string> annotations;

		annotations.insert({"OBS bcrash", FormatVAString(format, args)});

		bool rc = crashpadInfo->client.StartHandler(
		    crashpadInfo->handler,
		    crashpadInfo->db,
		    crashpadInfo->db,
		    crashpadInfo->url,
		    annotations,
		    crashpadInfo->arguments,
		    true,
		    true);

		rc = crashpadInfo->client.WaitForHandlerStart(INFINITE);

		throw "Induced obs crash";

	}, &crashpadInfo);

#endif

	// Initialize Server
	try {
		myServer.initialize(argv[1]);
	} catch (std::exception& e) {
		std::cerr << "Initialization failed with error " << e.what() << "." << std::endl;
		return -2;
	} catch (...) {
		std::cerr << "Failed to initialize server" << std::endl;
		return -2;
	}

	// Reset Connect/Disconnect time.
	sd.last_disconnect = sd.last_connect = std::chrono::high_resolution_clock::now();

	bool waitBeforeClosing = false;

	while (!doShutdown) {
		if (sd.count_connected == 0) {
			auto tp    = std::chrono::high_resolution_clock::now();
			auto delta = tp - sd.last_disconnect;
			if (std::chrono::duration_cast<std::chrono::milliseconds>(delta).count() > 5000) {
				doShutdown = true;
				waitBeforeClosing = true;
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}

	// Wait on receive the exit message from the crash-handler
	if (waitBeforeClosing) {
		HANDLE hPipe;
		TCHAR  chBuf[BUFFSIZE];
		DWORD  cbRead;
		hPipe = CreateNamedPipe(
		    TEXT("\\\\.\\pipe\\exit-slobs-crash-handler"),
		    PIPE_ACCESS_DUPLEX,
		    PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
		    1,
		    BUFFSIZE * sizeof(TCHAR),
		    BUFFSIZE * sizeof(TCHAR),
		    NULL,
		    NULL);

		if (hPipe != INVALID_HANDLE_VALUE) {
			if (ConnectNamedPipe(hPipe, NULL) != FALSE) {
				BOOL fSuccess = ReadFile(hPipe, chBuf, BUFFSIZE * sizeof(TCHAR), &cbRead, NULL);

				if (!fSuccess)
					return 0;
				CloseHandle(hPipe);
			}
		}
	}

	osn::Source::finalize_global_signals();
	OBS_API::destroyOBS_API();

	// Finalize Server
	myServer.finalize();

	return 0;
}
