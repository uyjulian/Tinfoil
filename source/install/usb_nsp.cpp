#include "install/usb_nsp.hpp"

extern "C"
{
#include <switch/services/hid.h>
#include <switch/arm/counter.h>
#include <switch/kernel/svc.h>
#include <switch/runtime/devices/console.h>
}

#include <algorithm>
#include <malloc.h>
#include "data/byte_buffer.hpp"
#include "data/buffered_placeholder_writer.hpp"
#include "util/usb_util.hpp"
#include "error.hpp"
#include "debug.h"

namespace tin::install::nsp
{
    USBNSP::USBNSP(std::string nspName) :
        m_nspName(nspName)
    {

    }

    struct USBFuncArgs
    {
        std::string nspName;
        tin::data::BufferedPlaceholderWriter* bufferedPlaceholderWriter;
        u64 pfs0Offset;
        u64 ncaSize;
    };

    void USBNSP::StreamToPlaceholder(nx::ncm::ContentStorage& contentStorage, NcmNcaId placeholderId)
    {
        const PFS0FileEntry* fileEntry = this->GetFileEntryByNcaId(placeholderId);
        std::string ncaFileName = this->GetFileEntryName(fileEntry);

        LOG_DEBUG("Retrieving %s\n", ncaFileName.c_str());
        size_t ncaSize = fileEntry->fileSize;

        tin::data::BufferedPlaceholderWriter bufferedPlaceholderWriter(&contentStorage, placeholderId, ncaSize);

        u64 freq = armGetSystemTickFreq();
        u64 startTime = armGetSystemTick();
        size_t startSizeBuffered = 0;
        double speed = 0.0;
        u64 totalSizeMB = bufferedPlaceholderWriter.GetTotalDataSize() / 1000000;

        tin::util::USBCmdHeader header = tin::util::USBCmdManager::SendFileRangeCmd(CMD_ID_FILE_RANGE_PADDED, m_nspName, this->GetDataOffset() + fileEntry->dataOffset, ncaSize);

        u8* buf = (u8*)memalign(0x1000, tin::data::BUFFER_SEGMENT_DATA_SIZE);
        u64 sizeRemaining = header.dataSize;
        size_t tmpSizeRead = 0;

        try
        {
            while (sizeRemaining)
            {
                size_t padding = 0;
                if (header.cmdId == CMD_ID_FILE_RANGE_PADDED)
                    padding = PADDING_SIZE;
                
                tmpSizeRead = usbCommsRead(buf, std::min(sizeRemaining + padding, tin::data::BUFFER_SEGMENT_DATA_SIZE)) - padding;
                sizeRemaining -= tmpSizeRead;
                if (bufferedPlaceholderWriter.CanAppendData(tmpSizeRead))
                    bufferedPlaceholderWriter.AppendData(buf + padding, tmpSizeRead);
                if (bufferedPlaceholderWriter.CanWriteSegmentToPlaceholder())
                    bufferedPlaceholderWriter.WriteSegmentToPlaceholder();

                u64 newTime = armGetSystemTick();
                if (newTime - startTime >= freq)
                {
                    size_t newSizeBuffered = bufferedPlaceholderWriter.GetSizeBuffered();
                    double mbBuffered = (newSizeBuffered / 1000000.0) - (startSizeBuffered / 1000000.0);
                    double duration = ((double)(newTime - startTime) / (double)freq);
                    speed = mbBuffered / duration;

                    startTime = newTime;
                    startSizeBuffered = newSizeBuffered;
                }

                u64 installSizeMB = bufferedPlaceholderWriter.GetSizeWrittenToPlaceholder() / 1000000;
                int installProgress = (int)(((double)bufferedPlaceholderWriter.GetSizeWrittenToPlaceholder() / (double)bufferedPlaceholderWriter.GetTotalDataSize()) * 100.0);
                printf("> Install Progress: %lu/%lu MB (%i%s) (%.2f MB/s)\r", installSizeMB, totalSizeMB, installProgress, "%", speed);

                consoleUpdate(NULL);
            }
        }
        catch (std::exception& e)
        {
            printf("An error occurred:\n%s\n", e.what());
            LOG_DEBUG("An error occurred:\n%s", e.what());
            consoleUpdate(NULL);
        }

        free(buf);
    }

    void USBNSP::BufferData(void* buf, off_t offset, size_t size)
    {
        tin::util::USBCmdHeader header = tin::util::USBCmdManager::SendFileRangeCmd(CMD_ID_FILE_RANGE, m_nspName, offset, size);
        tin::util::USBRead(buf, header.dataSize);
    }
}