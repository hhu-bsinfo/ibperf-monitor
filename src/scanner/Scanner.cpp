/*
 * Copyright (C) 2018 Heinrich-Heine-Universitaet Duesseldorf,
 * Institute of Computer Science, Department Operating Systems
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */

#include <ncurses.h>
#include <detector/BuildConfig.h>
#include <detector/exception/IbMadException.h>
#include <detector/exception/IbFileException.h>
#include <curses/YesNoMessageWindow.h>
#include <verbs.h>
#include <mad.h>
#include "curses/WindowManager.h"
#include "curses/OkMessageWindow.h"
#include "BuildConfig.h"
#include "MonitorWindow.h"
#include "Scanner.h"

namespace Scanner {

Scanner::Scanner(bool network, bool compatibility) :
        m_diagPerfCounterMap(std::unordered_map<uint64_t, Detector::IbDiagPerfCounter*>()),
        m_fabric(nullptr),
        m_manager(Curses::WindowManager::GetInstance()),
        m_helpWindow(nullptr),
        m_menuWindow(nullptr),
        m_oldStderr(dup(2)),
        m_network(network),
        m_compatibility(compatibility),
        m_isRunning(true)
{
    snprintf(m_helpMessage, 512, "ib-scanner %s - git %s(%s)\n"
                                 "Build date: %s\n"
                                 "Copyright (C) 2018 Heinrich-Heine-Universitaet Duesseldorf,\n"
                                 "Institute of Computer Science, Department Operating Systems\n"
                                 "Licensed under GPL v3\n\n"
                                 "Build against:\n"
                                 "Detector %s - git %s(%s)\n"
                                 "Build date: %s\n\n"
                                 "Up/Down: Navigate menu\n"
                                 "Right/Left: Open/Close menu entry\n"
                                 "Enter: Select for single view\n"
                                 "1/2/3/4: Assign to window\n"
                                 "Tab: Switch window", BuildConfig::VERSION, BuildConfig::GIT_REV,
                                 BuildConfig::GIT_BRANCH, BuildConfig::BUILD_DATE, Detector::BuildConfig::VERSION,
                                 Detector::BuildConfig::GIT_REV, Detector::BuildConfig::GIT_BRANCH,
                                 Detector::BuildConfig::BUILD_DATE);
}

Scanner::~Scanner() {
    delete m_helpWindow;
    delete m_menuWindow;
    delete m_fabric;

    delete m_monitorWindow[0];
    delete m_monitorWindow[1];
    delete m_monitorWindow[2];
    delete m_monitorWindow[3];

    for(const auto &entry : m_diagPerfCounterMap) {
        delete entry.second;
    }

    fdopen(m_oldStderr, "w");
}

void Scanner::Run() {
    // Suppress all messages, that are printed to stderr.
    // Unfortunately, this is needed, because libibmad prints warning to stderr.
    // These warnings cannot be turned off and intercept with ncurses.
    freopen("/dev/null", "w", stderr);

    m_manager->Initialize();

    m_helpWindow = new Curses::OkMessageWindow("Help", m_helpMessage, [] {});

    m_manager->Start();

    ScanFabric();

    m_manager->AddMenuFunction("Help", [&] { m_manager->RegisterWindow(m_helpWindow); });
    m_manager->AddMenuFunction("Reset Counters", [&] {
        m_monitorWindow[0]->ResetValues();
        m_monitorWindow[1]->ResetValues();
        m_monitorWindow[2]->ResetValues();
        m_monitorWindow[3]->ResetValues();
        m_manager->RequestRefresh();
    });
    m_manager->AddMenuFunction("Single Window", [&] {
        SetWindowCount(1);
        m_manager->SetFocus(m_menuWindow);
    });
    m_manager->AddMenuFunction("Dual Window", [&] {
        SetWindowCount(2);
        m_manager->SetFocus(m_menuWindow);
    });
    m_manager->AddMenuFunction("Quad Window", [&] {
        SetWindowCount(4);
        m_manager->SetFocus(m_menuWindow);
    });
    m_manager->AddMenuFunction("Exit", [&] { m_isRunning = false; });

    StartMonitoring();

    m_manager->DeregisterWindow(m_helpWindow);

    m_manager->Stop();
}

void Scanner::ScanFabric() {
    char doneMsgBuf[100];

    Curses::MessageWindow scanMsg("scanner", "Scanning fabric! Please wait...");
    m_manager->RegisterWindow(&scanMsg);

    // Scan for local devices and create an instance of Detector::IbDiagPerfCounter for each found device
    int numDevices;
    ibv_device **deviceList = ibv_get_device_list(&numDevices);

    if(deviceList == nullptr) {
        printf("Unable to get device list! Error: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    for(int32_t i = 0; i < numDevices; i++) {
        const char *deviceName = ibv_get_device_name(deviceList[i]);
        ibv_context *deviceContext = ibv_open_device(deviceList[i]);

        if(deviceContext == nullptr) {
            continue;
        }

        ibv_device_attr deviceAttributes{};
        int ret = ibv_query_device(deviceContext, &deviceAttributes);

        if(ret != 0) {
            ibv_close_device(deviceContext);
            continue;
        }

        m_diagPerfCounterMap[ntohll(deviceAttributes.node_guid)] = new Detector::IbDiagPerfCounter(deviceName, 0);

        for(uint8_t j = 1; j < deviceAttributes.phys_port_cnt + 1; j++) {
            ibv_port_attr portAttributes{};
            ret = ibv_query_port(deviceContext, j, &portAttributes);

            if(ret != 0) {
                continue;
            }

            m_diagPerfCounterMap[portAttributes.lid] = new Detector::IbDiagPerfCounter(deviceName, j);
        }

        ibv_close_device(deviceContext);
    }

    ibv_free_device_list(deviceList);

    //Scan the entire fabric for devices
    try {
        m_fabric = new Detector::IbFabric(m_network, m_compatibility);
    } catch (const Detector::IbMadException &exception) {
        bool wait = true;

        Curses::YesNoMessageWindow errorWindow("Error", "An error occurred, while scanning the fabric.\n"
                                                           "You probably don't have root privileges.\n"
                                                           "Do you want to continue in compatibility mode?\n"
                                                           "You will only be able to scan local devices,\n"
                                                           "but don't need root prvilieges.",
                                                           [&](bool sel) {
            if(sel){
                m_compatibility = true;

                delete m_fabric;
                m_fabric = new Detector::IbFabric(false, m_compatibility);

                wait = false;
            } else {
                m_manager->Stop();
                exit(EXIT_FAILURE);
            }
        });

        m_manager->RegisterWindow(&errorWindow);

        while(wait);
    } catch (const Detector::IbFileException &exception) {
        m_manager->Stop();
        exit(EXIT_FAILURE);
    }

    m_manager->DeregisterWindow(&scanMsg);

    bool wait = true;

    snprintf(doneMsgBuf, 100, "Finished scanning fabric! %d nodes found.", m_fabric->GetNumNodes());

    Curses::OkMessageWindow doneMsg("scanner", doneMsgBuf, [&] {
        wait = false;

        if(m_fabric->GetNumNodes() == 0) {
            m_manager->Stop();

            exit(EXIT_SUCCESS);
        }
    });

    Curses::WindowManager::GetInstance()->RegisterWindow(&doneMsg);

    while(wait);
}

void Scanner::StartMonitoring() {
    uint32_t termWidth = m_manager->GetTerminalWidth();
    uint32_t termHeight = m_manager->GetTerminalHeight();

    m_menuWindow = new Curses::MenuWindow(0, 0, 70, termHeight - 1, "Menu");

    Detector::IbDiagPerfCounter *diagPerfCounter = nullptr;

    if(m_diagPerfCounterMap.find(m_fabric->GetNodes()[0]->GetGuid()) != m_diagPerfCounterMap.end()) {
        diagPerfCounter = m_diagPerfCounterMap[m_fabric->GetNodes()[0]->GetGuid()];
    }

    m_monitorWindow[0] = new MonitorWindow(70, 0, termWidth - 70, termHeight - 1,
            m_fabric->GetNodes()[0]->GetDescription().c_str(), m_fabric->GetNodes()[0], diagPerfCounter);
    m_monitorWindow[1] = new MonitorWindow(70, 0, termWidth - 70, termHeight - 1,
            m_fabric->GetNodes()[0]->GetDescription().c_str(), m_fabric->GetNodes()[0], diagPerfCounter);
    m_monitorWindow[2] = new MonitorWindow(70, 0, termWidth - 70, termHeight - 1,
            m_fabric->GetNodes()[0]->GetDescription().c_str(), m_fabric->GetNodes()[0], diagPerfCounter);
    m_monitorWindow[3] = new MonitorWindow(70, 0, termWidth - 70, termHeight - 1,
            m_fabric->GetNodes()[0]->GetDescription().c_str(), m_fabric->GetNodes()[0], diagPerfCounter);

    m_menuWindow->AddKeyHandler('1', [&]() {
        Curses::MenuItem &item = m_menuWindow->GetSelectedItem();

        Detector::IbPerfCounter* perfCounter = reinterpret_cast<Detector::IbPerfCounter*>(item.GetData());
        Detector::IbDiagPerfCounter* diagCounter = m_diagPerfCounterMap[reinterpret_cast<uint64_t>(perfCounter)];

        m_monitorWindow[0]->SetPerfCounter(perfCounter, diagCounter);
        m_monitorWindow[0]->SetTitle(item.GetName());
    });

    m_menuWindow->AddKeyHandler('2', [&]() {
        Curses::MenuItem &item = m_menuWindow->GetSelectedItem();

        Detector::IbPerfCounter* perfCounter = reinterpret_cast<Detector::IbPerfCounter*>(item.GetData());
        Detector::IbDiagPerfCounter* diagCounter = m_diagPerfCounterMap[reinterpret_cast<uint64_t>(perfCounter)];

        m_monitorWindow[1]->SetPerfCounter(perfCounter, diagCounter);
        m_monitorWindow[1]->SetTitle(item.GetName());
    });

    m_menuWindow->AddKeyHandler('3', [&]() {
        Curses::MenuItem &item = m_menuWindow->GetSelectedItem();

        Detector::IbPerfCounter* perfCounter = reinterpret_cast<Detector::IbPerfCounter*>(item.GetData());
        Detector::IbDiagPerfCounter* diagCounter = m_diagPerfCounterMap[reinterpret_cast<uint64_t>(perfCounter)];

        m_monitorWindow[2]->SetPerfCounter(perfCounter, diagCounter);
        m_monitorWindow[2]->SetTitle(item.GetName());
    });

    m_menuWindow->AddKeyHandler('4', [&]() {
        Curses::MenuItem &item = m_menuWindow->GetSelectedItem();

        Detector::IbPerfCounter* perfCounter = reinterpret_cast<Detector::IbPerfCounter*>(item.GetData());
        Detector::IbDiagPerfCounter* diagCounter = m_diagPerfCounterMap[reinterpret_cast<uint64_t>(perfCounter)];

        m_monitorWindow[3]->SetPerfCounter(perfCounter, diagCounter);
        m_monitorWindow[3]->SetTitle(item.GetName());
    });

    for (Detector::IbNode *node : m_fabric->GetNodes()) {
        Detector::IbDiagPerfCounter *nodeDiagPerfCounter = nullptr;

        if(m_diagPerfCounterMap.find(node->GetGuid()) != m_diagPerfCounterMap.end()) {
            nodeDiagPerfCounter = m_diagPerfCounterMap[node->GetGuid()];

            m_diagPerfCounterMap.erase(node->GetGuid());
            m_diagPerfCounterMap[reinterpret_cast<uint64_t>(node)] = nodeDiagPerfCounter;
        }

        Curses::MenuItem item(node->GetDescription().c_str(), [&, node, nodeDiagPerfCounter]() {
            SetWindowCount(1);

            m_manager->SetFocus(m_menuWindow);

            m_monitorWindow[0]->SetPerfCounter(node, nodeDiagPerfCounter);
            m_monitorWindow[0]->SetTitle(node->GetDescription().c_str());

            m_manager->RequestRefresh();
        }, node);

        for (Detector::IbPort *port : node->GetPorts()) {
            char portName[10];
            snprintf(portName, 10, "Port %d", unsigned(port->GetNum()));

            Detector::IbDiagPerfCounter *portDiagPerfCounter = nullptr;

            if(m_diagPerfCounterMap.find(port->GetLid()) != m_diagPerfCounterMap.end()) {
                portDiagPerfCounter = m_diagPerfCounterMap[port->GetLid()];

                m_diagPerfCounterMap.erase(port->GetLid());
                m_diagPerfCounterMap[reinterpret_cast<uint64_t>(port)] = portDiagPerfCounter;
            }

            item.AddSubitem(Curses::MenuItem(portName, [&, port, portName, portDiagPerfCounter]() {
                SetWindowCount(1);

                m_manager->SetFocus(m_menuWindow);

                m_monitorWindow[0]->SetPerfCounter(port, portDiagPerfCounter);
                m_monitorWindow[0]->SetTitle(portName);

                m_manager->RequestRefresh();
            }, port));
        }

        m_menuWindow->AddItem(item);
    }

    m_manager->RegisterWindow(m_monitorWindow[0]);
    m_manager->RegisterWindow(m_menuWindow);

    while (m_isRunning);

    m_manager->DeregisterWindow(m_menuWindow);
    m_manager->DeregisterWindow(m_monitorWindow[0]);
    m_manager->DeregisterWindow(m_monitorWindow[1]);
    m_manager->DeregisterWindow(m_monitorWindow[2]);
    m_manager->DeregisterWindow(m_monitorWindow[3]);
}

void Scanner::SetWindowCount(uint8_t windowCount) {
    uint32_t termWidth = m_manager->GetTerminalWidth();
    uint32_t termHeight = m_manager->GetTerminalHeight();

    m_manager->DeregisterWindow(m_monitorWindow[0]);
    m_manager->DeregisterWindow(m_monitorWindow[1]);
    m_manager->DeregisterWindow(m_monitorWindow[2]);
    m_manager->DeregisterWindow(m_monitorWindow[3]);

    if(windowCount == 1) {
        m_manager->RegisterWindow(m_monitorWindow[0]);

        m_monitorWindow[0]->Move(70, 0);
        m_monitorWindow[0]->Resize(termWidth - 70, (termHeight - 1));
    } else if(windowCount == 2) {
        m_manager->RegisterWindow(m_monitorWindow[1]);
        m_manager->RegisterWindow(m_monitorWindow[0]);

        m_monitorWindow[0]->Move(70, 0);
        m_monitorWindow[0]->Resize(termWidth - 70, (termHeight - 1) / 2);
        m_monitorWindow[1]->Move(70, (termHeight - 1) / 2);
        m_monitorWindow[1]->Resize(termWidth - 70, (termHeight - 1) / 2);
    } else if(windowCount == 4) {
        m_manager->RegisterWindow(m_monitorWindow[3]);
        m_manager->RegisterWindow(m_monitorWindow[2]);
        m_manager->RegisterWindow(m_monitorWindow[1]);
        m_manager->RegisterWindow(m_monitorWindow[0]);

        m_monitorWindow[0]->Move(70, 0);
        m_monitorWindow[0]->Resize(termWidth - 70, (termHeight - 1) / 4);
        m_monitorWindow[1]->Move(70, (termHeight - 1) / 4);
        m_monitorWindow[1]->Resize(termWidth - 70, (termHeight - 1) / 4);
        m_monitorWindow[2]->Move(70, (termHeight - 1) / 2);
        m_monitorWindow[2]->Resize(termWidth - 70, (termHeight - 1) / 4);
        m_monitorWindow[3]->Move(70, (termHeight - 1) - (termHeight - 1) / 4);
        m_monitorWindow[3]->Resize(termWidth - 70, (termHeight - 1) / 4);
    }

    Curses::WindowManager::GetInstance()->RequestRefresh();
}

}

bool network = true;
bool compat = false;

void printUsage() {
    printf("Usage: ./scanner [OPTION]...\n"
           "Available options:\n"
           "-s, --scan\n"
           "    Set where to scan for devices, possible values are 'network' and 'local' (Default: 'network').\n"
           "-m, --mode\n"
           "    Set the operating mode to either 'mad' or 'compat' (Default: 'mad').\n"
           "-h, --help\n"
           "    Show this help message.\n");
}

void parseOpts(int argc, char *argv[]) {
    while(argc > 0) {
        if(!strcmp(argv[0], "-s") || !(strcmp(argv[0], "--scan"))) {
            if(argc < 2) {
                printUsage();

                printf("\n'%s' requires a parameter!\n", argv[0]);

                exit(EXIT_FAILURE);
            }

            if(!strcmp(argv[1], "network")) {
                network = true;
            } else if(!strcmp(argv[1], "local")) {
                network = false;
            } else {
                printUsage();

                printf("\nUnrecognized parameter '%s' for option '%s'!\n", argv[1], argv[0]);

                exit(EXIT_FAILURE);
            }
        } else if(!strcmp(argv[0], "-m") || !(strcmp(argv[0], "--mode"))) {
            if(argc < 2) {
                printUsage();

                printf("\n'%s' requires a parameter!\n", argv[0]);

                exit(EXIT_FAILURE);
            }

            if(!strcmp(argv[1], "mad")) {
                compat = false;
            } else if(!strcmp(argv[1], "compat")) {
                compat = true;
            } else {
                printUsage();

                printf("\nUnrecognized parameter '%s' for option '%s'!\n", argv[1], argv[0]);

                exit(EXIT_FAILURE);
            }
        } else if(!strcmp(argv[0], "-h") || !(strcmp(argv[0], "--help"))) {
            printUsage();

            exit(EXIT_SUCCESS);
        } else {
            printUsage();

            printf("\nUnknown option '%s'!\n", argv[0]);

            exit(EXIT_FAILURE);
        }

        argc -= 2;
        argv = &argv[2];
    }
}

int main(int argc, char *argv[]) {
    parseOpts(argc - 1, &argv[1]);

    Scanner::Scanner perfMon(network, compat);

    perfMon.Run();

    exit(EXIT_SUCCESS);
}