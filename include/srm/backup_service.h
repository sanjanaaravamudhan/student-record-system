

#pragma once

#include "srm/common_types.h"

#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <vector>

namespace srm {

class AuditService;

class BackupService {
public:
    using CopyFileOperation = std::function<bool(
        const std::filesystem::path&,
        const std::filesystem::path&,
        std::error_code&)>;

    BackupService(
        std::filesystem::path dataDirectory,
        std::filesystem::path backupDirectory,
        AuditService* auditService = nullptr,
        CopyFileOperation copyFileOperation = {});

    // SRM-F-041: Create a timestamped backup of system data.
    OperationResult createBackup(const Session& session);

    // List available backup directories.
    std::vector<std::filesystem::path> listBackups() const;

    // SRM-F-042: Restore a selected backup after explicit confirmation.
    OperationResult restoreBackup(
        const Session& session,
        const std::string& backupName,
        const std::string& confirmation);

    void setAuditService(AuditService* auditService);

private:
    std::filesystem::path dataDirectory_;
    std::filesystem::path backupDirectory_;
    AuditService* auditService_;
    CopyFileOperation copyFileOperation_;

    std::string generateBackupName() const;

    bool isValidBackupName(const std::string& backupName) const;

    bool resolveBackupPath(
        const std::string& backupName,
        std::filesystem::path& backupPath) const;

    bool createUniqueBackupDirectory(
        std::string& backupName,
        std::filesystem::path& backupPath,
        std::error_code& error) const;

    bool validateBackup(
        const std::filesystem::path& backupPath) const;

    bool copyFile(
        const std::filesystem::path& source,
        const std::filesystem::path& destination,
        std::error_code& error) const;

    bool copyRegularFiles(
        const std::filesystem::path& sourceDirectory,
        const std::filesystem::path& destinationDirectory,
        bool excludeAuditLog,
        std::error_code& error) const;

    bool removeBackupDirectory(
        const std::filesystem::path& path,
        std::error_code& error) const;

    bool prepareRollback(
        const std::string& backupName,
        std::filesystem::path& rollbackPath,
        std::error_code& error) const;

    bool removeObsoleteFiles(
        const std::filesystem::path& backupPath,
        std::error_code& error) const;

    bool restoreFiles(
        const std::filesystem::path& backupPath,
        std::error_code& error) const;

    bool rollbackRestore(
        const std::filesystem::path& rollbackPath,
        std::error_code& error) const;
};

}  // namespace srm
