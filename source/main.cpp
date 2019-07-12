#include <stdio.h>
#include <switch.h>

#include <exception>
#include <sstream>
#include <stdlib.h>
#include <malloc.h>
#include <threads.h>
#include <unistd.h>
#include <stdexcept>
#include <memory>
#include "data/byte_buffer.hpp"
#include "install/install_nsp_remote.hpp"
#include "install/usb_nsp.hpp"
#include "util/usb_util.hpp"
#include "debug.h"
#include "error.hpp"

#include <sys/socket.h>
#include <arpa/inet.h>

#include "nx/ipc/tin_ipc.h"

#include "debug.h"
#include "error.hpp"

#ifdef __cplusplus
extern "C" {
#endif

void userAppInit(void);
void userAppExit(void);

#ifdef __cplusplus
}
#endif

// TODO: Make custom error codes in some sort of logical fashion
// TODO: Create a proper logging setup, as well as a log viewing screen
// TODO: Validate NCAs
// TODO: Verify dumps, ncaids match sha256s, installation succeess, perform proper uninstallation on failure and prior to install

bool g_shouldExit = false;

void userAppInit(void)
{
    if (R_FAILED(ncmextInitialize()))
        fatalSimple(0xBEEF);

    if (R_FAILED(ncmInitialize()))
        fatalSimple(0xBEE2);

    if (R_FAILED(nsInitialize()))
        fatalSimple(0xBEE3);

    if (R_FAILED(nsextInitialize()))
        fatalSimple(0xBEE4);

    if (R_FAILED(esInitialize()))
        fatalSimple(0xBEE5);

    if (R_FAILED(nifmInitialize()))
        fatalSimple(0xBEE6);

    if (R_FAILED(setInitialize()))
        fatalSimple(0xBEE7);

    if (R_FAILED(plInitialize()))
        fatalSimple(0xBEE8);

    if (R_FAILED(usbCommsInitialize()))
        fatalSimple(0xBEEA);

    // We initialize this inside ui_networkinstall_mode for normal users.
    #ifdef NXLINK_DEBUG
    socketInitializeDefault();
    nxLinkInitialize();
    #endif
}

void userAppExit(void)
{
    nifmExit();

    #ifdef NXLINK_DEBUG
    nxLinkExit();
    socketExit();
    #endif

    usbCommsExit();
    plExit();
    setExit();
    ncmextExit();
    ncmExit();
    nsExit();
    nsextExit();
    esExit();
}

void markForExit(void)
{
    g_shouldExit = true;
}

struct TUSHeader
{
    u32 magic; // TUL0 (Tinfoil Usb List 0)
    u32 nspListSize;
    u64 padding;
} PACKED;

int main(int argc, char **argv)
{
    consoleInit(NULL);

    try
    {
        FsStorageId destStorageId = FsStorageId_SdCard;
        bool ignoreReqFirmVersion = false;
        Result rc = 0;
        printf("Waiting for USB to be ready...\n");

        consoleUpdate(NULL);

        while (true)
        {
            hidScanInput();
            
            if (hidKeysDown(CONTROLLER_P1_AUTO) & KEY_B)
                break;

            rc = usbDsWaitReady(1000000);

            if (R_SUCCEEDED(rc)) break;
            else if ((rc & 0x3FFFFF) != 0xEA01)
            {
                // Timeouts are okay, we just want to allow users to escape at this point
                THROW_FORMAT("Failed to wait for USB to be ready\n"); 
            }   
        }

        printf("USB is ready. Waiting for header...\n");

        consoleUpdate(NULL);
        
        TUSHeader header;
        tin::util::USBRead(&header, sizeof(TUSHeader));

        if (header.magic != 0x304C5554)
            THROW_FORMAT("Incorrect TUL header magic!\n");

        LOG_DEBUG("Valid header magic.\n");
        LOG_DEBUG("NSP List Size: %u\n", header.nspListSize);

        auto nspListBuf = std::make_unique<char[]>(header.nspListSize+1);
        std::vector<std::string> nspNames;
        memset(nspListBuf.get(), 0, header.nspListSize+1);

        tin::util::USBRead(nspListBuf.get(), header.nspListSize);

        // Split the string up into individual nsp names
        std::stringstream nspNameStream(nspListBuf.get());
        std::string segment;
        std::string nspExt = ".nsp";

        nspNames.clear();

        while (std::getline(nspNameStream, segment, '\n'))
        {
            if (segment.compare(segment.size() - nspExt.size(), nspExt.size(), nspExt) == 0)
                nspNames.push_back(segment);
        }

        destStorageId = FsStorageId_SdCard;
        // destStorageId = FsStorageId_NandUser;
        ignoreReqFirmVersion = false;

        for (auto& nspName : nspNames)
        {
            tin::install::nsp::USBNSP usbNSP(nspName);

            printf("Installing from %s\n", nspName.c_str());
            tin::install::nsp::RemoteNSPInstall install(destStorageId, ignoreReqFirmVersion, &usbNSP);

            printf("Preparing install...\n");
            install.Prepare();
            install.Begin();
            printf("\n");
        }

        tin::util::USBCmdManager::SendExitCmd();
    }
    catch (std::exception& e)
    {
        consoleClear();
        printf("An error occurred:\n%s\n\nPress any button to exit.", e.what());
        LOG_DEBUG("An error occurred:\n%s", e.what());

        u64 kDown = 0;

        while (!kDown)
        {
            hidScanInput();
            kDown = hidKeysDown(CONTROLLER_P1_AUTO);
        }
    }
    catch (...)
    {
        consoleClear();
        printf("An unknown error occurred\n\nPress any button to exit.");
        LOG_DEBUG("An unknown error occurred:\n");

        u64 kDown = 0;

        while (!kDown)
        {
            hidScanInput();
            kDown = hidKeysDown(CONTROLLER_P1_AUTO);
        }
    }

    consoleExit(NULL);
    return 0;
}