// Copyright (c) 2013- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

// Most of the code are based on https://github.com/RJ/libportfwd and updated to the latest miniupnp library
// All credit goes to him and the official miniupnp project! http://miniupnp.free.fr/

// Modifications by fzurita for M64Plus AE

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <mutex>

#include <android/log.h>
#include <libnatpmp/natpmp.h>

#include "PortManager.h"


PortManager g_PortManager;

PortManager::PortManager():
	urls(0),
	datas(0)
{

}

PortManager::~PortManager()
{
	// FIXME: On Windows it seems using any UPnP functions in this destructor that gets triggered when exiting PPSSPP will resulting to UPNPCOMMAND_HTTP_ERROR due to early WSACleanup (miniupnpc was getting WSANOTINITIALISED internally)
	Clear();
	Restore();
	Terminate();
}

void PortManager::Terminate()
{
    __android_log_write(ANDROID_LOG_VERBOSE, "miniupnp-bridge", "PortManager::Terminate()");

	if (urls) {
		FreeUPNPUrls(urls);
		free(urls);
		urls = NULL;
	}
	if (datas) {
		free(datas);
		datas = NULL;
	}
	m_otherPortList.clear(); m_otherPortList.shrink_to_fit();
	m_portList.clear(); m_portList.shrink_to_fit();
	m_lanip.clear();
	m_leaseDuration.clear();
	m_LocalPort = UPNP_LOCAL_PORT_ANY;
	m_InitState = UPNP_INITSTATE_NONE;
}

bool PortManager::Initialize(const unsigned int timeout)
{
	struct UPNPDev* devlist;
	struct UPNPDev* dev;
	char* descXML;
	int descXMLsize = 0;
	int descXMLstatus = 0;
	int localport = m_LocalPort; // UPNP_LOCAL_PORT_ANY (0), or UPNP_LOCAL_PORT_SAME (1) as an alias for 1900 (for backwards compatability?)
	int ipv6 = 0; // 0 = IPv4, 1 = IPv6
	unsigned char ttl = 2; // defaulting to 2
	int error = 0;

    __android_log_print(ANDROID_LOG_VERBOSE, "miniupnp-bridge", "PortManager::Initialize(%d)", timeout);

	if (m_InitState != UPNP_INITSTATE_NONE) {
		switch (m_InitState)
		{
		case UPNP_INITSTATE_BUSY: {
            __android_log_print(ANDROID_LOG_WARN, "miniupnp-bridge", "Initialization already in progress");

			return false;
		}
		// Should we redetect UPnP? just in case the player switched to a different network in the middle
		case UPNP_INITSTATE_DONE: {
            __android_log_print(ANDROID_LOG_WARN, "miniupnp-bridge", "Already Initialized");
			return true;
		}
		default:
			break;
		}
	}

	m_leaseDuration = "43200"; // 12 hours
	m_InitState = UPNP_INITSTATE_BUSY;
	urls = (UPNPUrls*)malloc(sizeof(struct UPNPUrls));
	datas = (IGDdatas*)malloc(sizeof(struct IGDdatas));
	memset(urls, 0, sizeof(struct UPNPUrls));
	memset(datas, 0, sizeof(struct IGDdatas));

	devlist = upnpDiscover(timeout, NULL, NULL, localport, ipv6, ttl, &error);

	if (devlist)
	{
		dev = devlist;
		while (dev)
		{
			if (strstr(dev->st, "InternetGatewayDevice"))
				break;

			__android_log_print(ANDROID_LOG_INFO, "miniupnp-bridge", "Found UPnP device: [desc: %s] [st: %s]", dev->descURL, dev->st);

			dev = dev->pNext;
		}
		if (!dev) {
			m_InitState = UPNP_INITSTATE_NONE;
			Terminate();
			__android_log_print(ANDROID_LOG_INFO, "miniupnp-bridge", "upnpDiscover failed (error: %i) or No UPnP device detected", error);

			return false;
		}

        __android_log_print(ANDROID_LOG_INFO, "miniupnp-bridge", "UPnP device: [desc: %s] [st: %s]", dev->descURL, dev->st);

		descXML = (char*)miniwget(dev->descURL, &descXMLsize, dev->scope_id, &descXMLstatus);
		if (descXML)
		{
			parserootdesc(descXML, descXMLsize, datas);
			free(descXML);
			GetUPNPUrls(urls, datas, dev->descURL, dev->scope_id);
		}

		// Get LAN IP address that connects to the router
		char lanaddr[64] = "unset";
		UPNP_GetValidIGD(devlist, urls, datas, lanaddr, sizeof(lanaddr)); //possible "status" values, 0 = NO IGD found, 1 = A valid connected IGD has been found, 2 = A valid IGD has been found but it reported as not connected, 3 = an UPnP device has been found but was not recognized as an IGD
		m_lanip = std::string(lanaddr);
        __android_log_print(ANDROID_LOG_INFO, "miniupnp-bridge", "Detected LAN IP: %s", m_lanip.c_str());

		// Additional Info
		char connectionType[64] = "";
		if (UPNP_GetConnectionTypeInfo(urls->controlURL, datas->first.servicetype, connectionType) != UPNPCOMMAND_SUCCESS) {
            __android_log_print(ANDROID_LOG_WARN, "miniupnp-bridge", "GetConnectionTypeInfo failed");
		}
		else {
            __android_log_print(ANDROID_LOG_INFO, "miniupnp-bridge", "Connection Type: %s", connectionType);
		}

		freeUPNPDevlist(devlist);

		m_InitState = UPNP_INITSTATE_DONE;
		RefreshPortList();
		return true;
	}

    __android_log_print(ANDROID_LOG_INFO, "miniupnp-bridge", "upnpDiscover failed (error: %i) or No UPnP device detected", error);

	m_InitState = UPNP_INITSTATE_NONE;
	Terminate();

	return false;
}

int PortManager::GetInitState()
{
	return m_InitState;
}

bool PortManager::Add(const char* protocol, const char* description, unsigned short port, unsigned short intport)
{
	char port_str[16];
	char intport_str[16];
	int r;

	if (intport == 0)
		intport = port;

    __android_log_print(ANDROID_LOG_INFO, "miniupnp-bridge", "PortManager::Add(%s, %d, %d)", protocol, port, intport);

	if (m_InitState != UPNP_INITSTATE_DONE)
	{
		__android_log_print(ANDROID_LOG_INFO, "miniupnp-bridge", "PortManager::Add UpnP device not initialized yet (%s, %d, %d)", protocol, port, intport);
		return false;
	}

	sprintf(port_str, "%d", port);
	sprintf(intport_str, "%d", intport);
	// Only add new port map if it's not previously created by PPSSPP for current IP
	auto el_it = std::find_if(m_portList.begin(), m_portList.end(),
		[port_str, protocol](const std::pair<std::string, std::string> &el) { return el.first == port_str && el.second == protocol; });
	if (el_it == m_portList.end()) {
		auto el_it_new = std::find_if(m_otherPortList.begin(), m_otherPortList.end(),
			[port_str, protocol](const PortMap& el) { return el.extPort_str == port_str && el.protocol == protocol; });
		if (el_it_new != m_otherPortList.end()) {
			// Try to delete the port mapping before we create it, just in case we have dangling port mapping from the daemon not being shut down correctly or the port was taken by other
			UPNP_DeletePortMapping(urls->controlURL, datas->first.servicetype, port_str, protocol, NULL);
		}
		r = UPNP_AddPortMapping(urls->controlURL, datas->first.servicetype,
			port_str, intport_str, m_lanip.c_str(), description, protocol, NULL, m_leaseDuration.c_str());
		if (r == 725 && m_leaseDuration != "0") {
			m_leaseDuration = "0";
			r = UPNP_AddPortMapping(urls->controlURL, datas->first.servicetype,
				port_str, intport_str, m_lanip.c_str(), description, protocol, NULL, m_leaseDuration.c_str());
		}
		if (r != 0)
		{
            __android_log_print(ANDROID_LOG_ERROR, "miniupnp-bridge", "AddPortMapping failed (error: %i)", r);

            if (r == UPNPCOMMAND_HTTP_ERROR) {
                Terminate(); // Most of the time errors occurred because the router is no longer reachable (ie. changed networks) so we should invalidate the state to prevent further lags due to timeouts
				return false;
			}
		}
		m_portList.push_front({ port_str, protocol });
		// Keep tracks of it to be restored later if it belongs to others
		if (el_it_new != m_otherPortList.end()) el_it_new->taken = true;
	}
	return true;
}

bool PortManager::Remove(const char* protocol, unsigned short port)
{
	char port_str[16];

    __android_log_print(ANDROID_LOG_INFO, "miniupnp-bridge", "PortManager::Remove(%s, %d)", protocol, port);

	if (m_InitState != UPNP_INITSTATE_DONE)
	{
		__android_log_print(ANDROID_LOG_INFO, "miniupnp-bridge", "PortManager::Remove UpnP device not initialized yet (%s, %d)", protocol, port);
		return false;
	}

	sprintf(port_str, "%d", port);
	int r = UPNP_DeletePortMapping(urls->controlURL, datas->first.servicetype, port_str, protocol, NULL);
	if (r != 0)
	{
        __android_log_print(ANDROID_LOG_ERROR, "miniupnp-bridge", "DeletePortMapping failed (error: %i)", r);

        if (r == UPNPCOMMAND_HTTP_ERROR) {
			Terminate(); // Most of the time errors occurred because the router is no longer reachable (ie. changed networks) so we should invalidate the state to prevent further lags due to timeouts
			return false;
		}
	}
	for (auto it = m_portList.begin(); it != m_portList.end(); ) {
		(it->first == port_str && it->second == protocol) ? it = m_portList.erase(it) : ++it;
	}
	return true;
}

bool PortManager::Restore()
{
	int r;
    __android_log_print(ANDROID_LOG_VERBOSE, "miniupnp-bridge", "PortManager::Restore()");

    if (m_InitState != UPNP_INITSTATE_DONE)
	{
        __android_log_print(ANDROID_LOG_WARN, "miniupnp-bridge", "PortManager::Remove - the init was not done !");

        return false;
	}
	for (auto it = m_otherPortList.begin(); it != m_otherPortList.end(); ++it) {
		if (it->taken) {
			auto port_str = it->extPort_str;
			auto protocol = it->protocol;
			// Remove it first if it's still being taken by PPSSPP
			auto el_it = std::find_if(m_portList.begin(), m_portList.end(),
				[port_str, protocol](const std::pair<std::string, std::string>& el) { return el.first == port_str && el.second == protocol; });
			if (el_it != m_portList.end()) {
				r = UPNP_DeletePortMapping(urls->controlURL, datas->first.servicetype, port_str.c_str(), protocol.c_str(), NULL);
				if (r == 0) {
					m_portList.erase(el_it);
				}
				else {
                    __android_log_print(ANDROID_LOG_ERROR, "miniupnp-bridge", "PortManager::Restore - DeletePortMapping failed (error: %i)", r);

                    if (r == UPNPCOMMAND_HTTP_ERROR)
						return false; // Might be better not to exit here, but exiting a loop will avoid long timeouts in the case the router is no longer reachable
				}
			}
			// Add the original owner back
			r = UPNP_AddPortMapping(urls->controlURL, datas->first.servicetype,
				it->extPort_str.c_str(), it->intPort_str.c_str(), it->lanip.c_str(), it->desc.c_str(), it->protocol.c_str(), it->remoteHost.c_str(), it->duration.c_str());
			if (r == 0) {
				it->taken = false;
			}
			else {
                __android_log_print(ANDROID_LOG_ERROR, "miniupnp-bridge", "PortManager::Restore - AddPortMapping failed (error: %i)", r);

                if (r == UPNPCOMMAND_HTTP_ERROR)
					return false; // Might be better not to exit here, but exiting a loop will avoid long timeouts in the case the router is no longer reachable
			}
		}
	}
	return true;
}

bool PortManager::Clear()
{
	int r;
	int i = 0;
	char index[6];
	char intAddr[40];
	char intPort[6];
	char extPort[6];
	char protocol[4];
	char desc[80];
	char enabled[6];
	char rHost[64];
	char duration[16];

    __android_log_print(ANDROID_LOG_VERBOSE, "miniupnp-bridge", "PortManager::Clear()");

    if (m_InitState != UPNP_INITSTATE_DONE)
	{
        __android_log_print(ANDROID_LOG_WARN, "miniupnp-bridge", "PortManager::Clear - the init was not done !");
		return false;
	}
	//unsigned int num = 0;
	//UPNP_GetPortMappingNumberOfEntries(urls->controlURL, datas->first.servicetype, &num); // Not supported by many routers
	do {
		snprintf(index, 6, "%d", i);
		rHost[0] = '\0'; enabled[0] = '\0';
		duration[0] = '\0'; desc[0] = '\0'; protocol[0] = '\0';
		extPort[0] = '\0'; intPort[0] = '\0'; intAddr[0] = '\0';
		// May gets UPNPCOMMAND_HTTP_ERROR when called while exiting PPSSPP (ie. used in destructor)
		r = UPNP_GetGenericPortMappingEntry(urls->controlURL,
			datas->first.servicetype,
			index,
			extPort, intAddr, intPort,
			protocol, desc, enabled,
			rHost, duration);
		// Only removes port mappings created by PPSSPP for current LAN IP
		if (r == 0 && intAddr == m_lanip && std::string(desc).find("M64Plus") != std::string::npos) {
			int r2 = UPNP_DeletePortMapping(urls->controlURL, datas->first.servicetype, extPort, protocol, rHost);
			if (r2 != 0)
			{
                __android_log_print(ANDROID_LOG_ERROR, "miniupnp-bridge", "PortManager::Clear - DeletePortMapping(%s, %s) failed (error: %i)", extPort, protocol, r2);

                if (r2 == UPNPCOMMAND_HTTP_ERROR)
					return false;
			}
			else {
				i--;
				for (auto it = m_portList.begin(); it != m_portList.end(); ) {
					(it->first == extPort && it->second == protocol) ? it = m_portList.erase(it) : ++it;
				}
			}
		}
		i++;
	} while (r == 0);
	return true;
}

bool PortManager::RefreshPortList()
{
	int r;
	int i = 0;
	char index[6];
	char intAddr[40];
	char intPort[6];
	char extPort[6];
	char protocol[4];
	char desc[80];
	char enabled[6];
	char rHost[64];
	char duration[16];

    __android_log_print(ANDROID_LOG_INFO, "miniupnp-bridge", "PortManager::RefreshPortList()");

    if (m_InitState != UPNP_INITSTATE_DONE)
	{
        __android_log_print(ANDROID_LOG_WARN, "miniupnp-bridge", "PortManager::RefreshPortList - the init was not done !");
        return false;
	}
	m_portList.clear();
	m_otherPortList.clear();
	//unsigned int num = 0;
	//UPNP_GetPortMappingNumberOfEntries(urls->controlURL, datas->first.servicetype, &num); // Not supported by many routers
	do {
		snprintf(index, 6, "%d", i);
		rHost[0] = '\0'; enabled[0] = '\0';
		duration[0] = '\0'; desc[0] = '\0'; protocol[0] = '\0';
		extPort[0] = '\0'; intPort[0] = '\0'; intAddr[0] = '\0';
		r = UPNP_GetGenericPortMappingEntry(urls->controlURL,
			datas->first.servicetype,
			index,
			extPort, intAddr, intPort,
			protocol, desc, enabled,
			rHost, duration);
		if (r == 0) {
			std::string desc_str = std::string(desc);
			// Some router might prefix the description with "UPnP:" so we may need to truncate it to prevent it from getting multiple prefix when restored later
			if (desc_str.find("UPnP:") == 0)
				desc_str = desc_str.substr(5);
			// Only include port mappings created by PPSSPP for current LAN IP
			if (intAddr == m_lanip && desc_str.find("M64Plus") != std::string::npos) {
				m_portList.push_back({ extPort, protocol });
			}
			// Port mappings belong to others that might be taken by PPSSPP later
			else {
				m_otherPortList.push_back({ false, protocol, extPort, intPort, intAddr, rHost, desc_str, duration, enabled });
			}
		}
		i++;
	} while (r == 0);
	return true;
}

extern "C" __attribute__((visibility("default"))) void UPnPInit(int timeout)
{
	if (g_PortManager.GetInitState() == UPNP_INITSTATE_NONE) {
		g_PortManager.Initialize(timeout);
	}
}

extern "C" __attribute__((visibility("default"))) void UPnPShutdown()
{
	// Cleaning up regardless of g_Config.bEnableUPnP to prevent lingering open ports on the router
	if (g_PortManager.GetInitState() == UPNP_INITSTATE_DONE) {
		g_PortManager.Clear();
		g_PortManager.Restore();
		g_PortManager.Terminate();
	}
}

extern "C" __attribute__((visibility("default"))) bool UPnP_Add(const char* protocol, const char* description, int port, int intport)
{
	static const int maxTries = 5;
	int currentTry = 0;
	while(!g_PortManager.Add(protocol, description, port, intport) && currentTry < maxTries)
	{
		++currentTry;
		__android_log_print(ANDROID_LOG_INFO, "miniupnp-bridge", "Add current try=%d", currentTry);
		std::this_thread::sleep_for (std::chrono::milliseconds (10));
	}

	return currentTry < maxTries;
}

extern "C" __attribute__((visibility("default"))) bool UPnP_Remove(const char* protocol, int port)
{
	__android_log_print(ANDROID_LOG_INFO, "miniupnp-bridge", "UPnP_Remove(%s, %d)", protocol, port);

	static const int maxTries = 5;
	int currentTry = 0;
	while(!g_PortManager.Remove(protocol, port) && currentTry < maxTries)
	{
		++currentTry;
		__android_log_print(ANDROID_LOG_INFO, "miniupnp-bridge", "Remove current try=%d", currentTry);
		std::this_thread::sleep_for (std::chrono::milliseconds (10));
	}

	return currentTry < maxTries;
}
