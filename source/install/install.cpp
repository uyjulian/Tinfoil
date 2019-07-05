#include "install/install.hpp"

#include <switch.h>
#include <cstring>
#include <memory>
#include "error.hpp"

#include "nx/ncm.hpp"
#include "util/title_util.hpp"


// TODO: Check NCA files are present
// TODO: Check tik/cert is present
namespace tin::install
{
    Install::Install(FsStorageId destStorageId, bool ignoreReqFirmVersion) :
        m_destStorageId(destStorageId), m_ignoreReqFirmVersion(ignoreReqFirmVersion), m_contentMeta()
    {}

    Install::~Install() {}

    // TODO: Implement RAII on NcmContentMetaDatabase
    void Install::InstallContentMetaRecords(tin::data::ByteBuffer& installContentMetaBuf)
    {
        NcmContentMetaDatabase contentMetaDatabase;
        NcmMetaRecord contentMetaKey = m_contentMeta.GetContentMetaKey();

        try
        {
            ASSERT_OK(ncmOpenContentMetaDatabase(m_destStorageId, &contentMetaDatabase), "Failed to open content meta database");
            ASSERT_OK(ncmContentMetaDatabaseSet(&contentMetaDatabase, &contentMetaKey, installContentMetaBuf.GetSize(), (NcmContentMetaRecordsHeader*)installContentMetaBuf.GetData()), "Failed to set content records");
            ASSERT_OK(ncmContentMetaDatabaseCommit(&contentMetaDatabase), "Failed to commit content records");
        }
        catch (std::runtime_error& e)
        {
            serviceClose(&contentMetaDatabase.s);
            throw e;
        }
		
        serviceClose(&contentMetaDatabase.s);
    }

    void Install::InstallApplicationRecord()
    {
        Result rc = 0;
        std::vector<ContentStorageRecord> storageRecords;
        u64 baseTitleId = tin::util::GetBaseTitleId(this->GetTitleId(), this->GetContentMetaType());
        u32 contentMetaCount = 0;

        LOG_DEBUG("Base title Id: 0x%lx", baseTitleId);

        // TODO: Make custom error with result code field
        // 0x410: The record doesn't already exist
        if (R_FAILED(rc = nsCountApplicationContentMeta(baseTitleId, &contentMetaCount)) && rc != 0x410)
        {
            throw std::runtime_error("Failed to count application content meta");
        }
        rc = 0;

        LOG_DEBUG("Content meta count: %u\n", contentMetaCount);

        // Obtain any existing app record content meta and append it to our vector
        if (contentMetaCount > 0)
        {
            storageRecords.resize(contentMetaCount);
            size_t contentStorageBufSize = contentMetaCount * sizeof(ContentStorageRecord);
            auto contentStorageBuf = std::make_unique<ContentStorageRecord[]>(contentMetaCount);
            u32 entriesRead;

            ASSERT_OK(nsListApplicationRecordContentMeta(0, baseTitleId, contentStorageBuf.get(), contentStorageBufSize, &entriesRead), "Failed to list application record content meta");

            if (entriesRead != contentMetaCount)
            {
                throw std::runtime_error("Mismatch between entries read and content meta count");
            }

            memcpy(storageRecords.data(), contentStorageBuf.get(), contentStorageBufSize);
        }

        // Add our new content meta
        ContentStorageRecord storageRecord;
        storageRecord.metaRecord = m_contentMeta.GetContentMetaKey();
        storageRecord.storageId = m_destStorageId;
        storageRecords.push_back(storageRecord);

        // Replace the existing application records with our own
        try
        {
            nsDeleteApplicationRecord(baseTitleId);
        }
        catch (...) {}

        printf("Pushing application record...\n");
        ASSERT_OK(nsPushApplicationRecord(baseTitleId, 0x3, storageRecords.data(), storageRecords.size() * sizeof(ContentStorageRecord)), "Failed to push application record");
    }

    // Validate and obtain all data needed for install
    void Install::Prepare()
    {
        tin::data::ByteBuffer cnmtBuf;
        auto cnmtTuple = this->ReadCNMT();
        m_contentMeta = std::get<0>(cnmtTuple);
        nx::ncm::ContentRecord cnmtContentRecord = std::get<1>(cnmtTuple);

        nx::ncm::ContentStorage contentStorage(m_destStorageId);

        if (!contentStorage.Has(cnmtContentRecord.ncaId))
        {
            printf("Installing CNMT NCA...\n");
            this->InstallNCA(cnmtContentRecord.ncaId);
        }
        else
        {
            printf("CNMT NCA already installed. Proceeding...\n");
        }

        // Parse data and create install content meta
        if (m_ignoreReqFirmVersion)
            printf("WARNING: Required system firmware version is being IGNORED!\n");

        tin::data::ByteBuffer installContentMetaBuf;
        m_contentMeta.GetInstallContentMeta(installContentMetaBuf, cnmtContentRecord, m_ignoreReqFirmVersion);

        this->InstallContentMetaRecords(installContentMetaBuf);
        this->InstallApplicationRecord();

        printf("Installing ticket and cert...\n");
        try
        {
            this->InstallTicketCert();
        }
        catch (std::runtime_error& e)
        {
            printf("WARNING: Ticket installation failed! This may not be an issue, depending on your use case.\nProceed with caution!\n");
        }
    }

    void Install::Begin()
    {
        printf("Installing NCAs...\n");
        for (auto& record : m_contentMeta.GetContentRecords())
        {
            LOG_DEBUG("Installing from %s\n", tin::util::GetNcaIdString(record.ncaId).c_str());
            this->InstallNCA(record.ncaId);
        }
    }

    u64 Install::GetTitleId()
    {
        return m_contentMeta.GetContentMetaKey().titleId;
    }

    nx::ncm::ContentMetaType Install::GetContentMetaType()
    {
        return static_cast<nx::ncm::ContentMetaType>(m_contentMeta.GetContentMetaKey().type);
    }
}
