/* 
    Copyright (C) 2009  Jiri Moskovcak (jmoskovc@redhat.com) 
    Copyright (C) 2009  RedHat inc. 
 
    This program is free software; you can redistribute it and/or modify 
    it under the terms of the GNU General Public License as published by 
    the Free Software Foundation; either version 2 of the License, or 
    (at your option) any later version. 
 
    This program is distributed in the hope that it will be useful, 
    but WITHOUT ANY WARRANTY; without even the implied warranty of 
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the 
    GNU General Public License for more details. 
 
    You should have received a copy of the GNU General Public License along 
    with this program; if not, write to the Free Software Foundation, Inc., 
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA. 
    */
    
#ifndef DBUS_H_
#define DBUS_H_

#include <string>
#include <dbus/dbus.h>
//#include <glibmm.h>
#include <dbus-c++/glib-integration.h>
#include <dbus-c++/dbus.h>

#define CC_DBUS_NAME "com.redhat.CrashCatcher"
#define CC_DBUS_PATH "/com/redhat/CrashCatcher"
#define CC_DBUS_IFACE "com.redhat.CrashCatcher"
#define DBUS_BUS DBUS_BUS_SYSTEM
#define CC_DBUS_PATH_NOTIFIER "/com/redhat/CrashCatcher/Crash"



class CDBusManager
{
    private:
        DBus::Glib::BusDispatcher *m_pDispatcher;
        DBus::Connection *m_pConn;
	public:
        CDBusManager();
        ~CDBusManager();
        void RegisterService();
        bool SendMessage(const std::string& pMessage, const std::string& pMessParam);
       /** TODO
        //tries to reconnect after daemon failure
        void Reconnect();
        */
};

#endif /*DBUS_H_*/
