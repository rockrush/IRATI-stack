/*
 * Mobility Manager (manages mobility locally in a system)
 *
 *    Eduard Grasa <eduard.grasa@i2cat.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA  02110-1301  USA
 */

#ifndef __MOBILITY_MANAGER_H__
#define __MOBILITY_MANAGER_H__

#include <librina/common.h>
#include <librina/ipc-manager.h>
#include <librina/timer.h>

#include "rina-configuration.h"
#include "../addon.h"
#include "../ipcp.h"

namespace rinad{

struct WirelessDIFInfo {
	std::string bs_address;
	std::string dif_name;
};

//
// @brief Handles the mobility of the system locally (decides when to
// change attachment to Base Stations, etc.)
//
//
class MobilityManager: public AppAddon {
public:
	static const std::string NAME;

	MobilityManager(const rinad::RINAConfiguration& config);
	virtual ~MobilityManager(void) {
	};

	int execute_handover(unsigned short ipcp_id,
			     const rina::ApplicationProcessNamingInformation& dif_name,
			     const rina::ApplicationProcessNamingInformation& neigh_name);

protected:
	//Process flow event
	void process_librina_event(rina::IPCEvent** event);

	//Process ipcm event
	void process_ipcm_event(const IPCMEvent& event);

private:
	IPCMIPCProcessFactory * factory;
	rina::Timer timer;
	std::list<WirelessDIFInfo> wireless_dif_info;

	void parse_configuration(const rinad::RINAConfiguration& config);

	//Called when a media report by an IPC Process has been delivered
	void media_reports_handler(rina::MediaReportEvent *event);
};

class TriggerHandoverTimerTask: public rina::TimerTask {
public:
	TriggerHandoverTimerTask(MobilityManager * mobility_manager,
				 rina::Timer * timer,
				 const WirelessDIFInfo& info);
	~TriggerHandoverTimerTask() throw() {};
	void run();

	MobilityManager * mobility_manager;
	rina::Timer * timer;
	WirelessDIFInfo dif_info;
};

}//rinad namespace

#endif  /* __MOBILITY_MANAGER_H__ */
