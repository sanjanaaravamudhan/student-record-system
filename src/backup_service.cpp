
#include "srm/backup_service.h"

#include "srm/audit_service.h"
#include "srm/rbac.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>
#include <utility>

namespace srm {

BackupService::BackupService(
    std::filesystem::path dataDirectory,
    std::filesystem::path backupDirectory,
    AuditService* auditService,
    CopyFileOperation copyFileOperation)
    : dataDirectory_(std::move(dataDirectory)),
      backupDirectory_(std::move(backupDirectory)),
      auditService_(auditService),
      copyFileOperation_(std::move(copyFileOperation)) {
    if (!copyFileOperation_) {
        copyFileOperation_ =
            [](const std::filesystem::path& source,
               const std::filesystem::path& destination,
               std::error_code& error) {
                std::filesystem::copy_file(
                    source,
                    destination,
                    std::filesystem::copy_options::overwrite_existing,
                    error);
                return !error;
            };
    }
}

void BackupService::setAuditService(AuditService* auditService) {
    auditService_ = auditService;
}

std::string BackupService::generateBackupName() const {
    const auto now = std::chrono::system_clock::now();
    const auto seconds =
        std::chrono::time_point_cast<std::chrono::seconds>(now);
    const auto microseconds =
        std::chrono::duration_cast<std::chrono::microseconds>(
            now - seconds).count();

    const std::time_t time =
        std::chrono::system_clock::to_time_t(seconds);

    std::tm localTime{};

#ifdef _WIN32
    localtime_s(&localTime, &time);
#else
    localtime_r(&time, &localTime);
#endif

    std::ostringstream name;
    name << "backup_" << std::put_time(&localTime, "%Y%m%d_%H%M%S")
         << "_" << std::setw(6) << std::setfill('0') << microseconds;

    return name.str();
}

bool BackupService::isValidBackupName(
    const std::string& backupName) const {
    static const std::regex backupNamePattern(
        R"(^backup_[0-9]{8}_[0-9]{6}(_[0-9]{6}(_[0-9]+)?)?$)");

    if (backupName.empty() ||
        backupName == "." ||
        backupName == "..") {
        return false;
    }

    const std::filesystem::path namePath(backupName);

    if (namePath.has_parent_path() ||
        namePath.filename() != namePath ||
        namePath == "." ||
        namePath == "..") {
        return false;
    }

    return std::regex_match(backupName, backupNamePattern);
}

bool BackupService::resolveBackupPath(
    const std::string& backupName,
    std::filesystem::path& backupPath) const {
    if (!isValidBackupName(backupName)) {
        return false;
    }

    std::error_code error;
    const auto canonicalDirectory =
        std::filesystem::weakly_canonical(backupDirectory_, error);

    if (error) {
        return false;
    }

    const auto candidate =
        std::filesystem::weakly_canonical(
            canonicalDirectory / backupName, error);

    if (error || candidate.parent_path() != canonicalDirectory) {
        return false;
    }

    backupPath = candidate;
    return true;
}

bool BackupService::createUniqueBackupDirectory(
    std::string& backupName,
    std::filesystem::path& backupPath,
    std::error_code& error) const {
    const std::string baseName = generateBackupName();

    for (unsigned int attempt = 0; attempt < 1000; ++attempt) {
        backupName = baseName;

        if (attempt > 0) {
            backupName += "_" + std::to_string(attempt);
        }

        backupPath = backupDirectory_ / backupName;
        error.clear();

        if (std::filesystem::create_directory(backupPath, error)) {
            return true;
        }

        if (error && error != std::errc::file_exists) {
            return false;
        }
    }

    error = std::make_error_code(std::errc::file_exists);
    return false;
}

std::vector<std::filesystem::path> BackupService::listBackups() const {
    std::vector<std::filesystem::path> backups;
    std::error_code error;

    if (!std::filesystem::exists(backupDirectory_, error) || error) {
        return backups;
    }

    for (const auto& entry :
         std::filesystem::directory_iterator(backupDirectory_, error)) {
        if (error) {
            break;
        }

        if (entry.is_directory(error) && !error &&
            isValidBackupName(entry.path().filename().string()) &&
            validateBackup(entry.path())) {
            backups.push_back(entry.path());
        }
    }

    std::sort(backups.begin(), backups.end());
    return backups;
}

bool BackupService::validateBackup(
    const std::filesystem::path& backupPath) const {
    std::error_code error;

    if (!std::filesystem::is_directory(backupPath, error) || error) {
        return false;
    }

    bool containsAtLeastOneFile = false;

    for (const auto& entry :
         std::filesystem::directory_iterator(backupPath, error)) {
        if (error) {
            return false;
        }

        if (!entry.is_regular_file(error) || error) {
            return false;
        }

        containsAtLeastOneFile = true;
    }

    return containsAtLeastOneFile;
}

bool BackupService::copyFile(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    std::error_code& error) const {
    return copyFileOperation_(source, destination, error);
}

bool BackupService::copyRegularFiles(
    const std::filesystem::path& sourceDirectory,
    const std::filesystem::path& destinationDirectory,
    bool excludeAuditLog,
    std::error_code& error) const {
    for (const auto& entry :
         std::filesystem::directory_iterator(sourceDirectory, error)) {
        if (error) {
            return false;
        }

        if (!entry.is_regular_file(error) || error) {
            if (error) {
                return false;
            }
            continue;
        }

        if (excludeAuditLog &&
            entry.path().filename() == "audit.log") {
            continue;
        }

        if (!copyFile(
                entry.path(),
                destinationDirectory / entry.path().filename(),
                error)) {
            return false;
        }
    }

    return !error;
}

bool BackupService::removeBackupDirectory(
    const std::filesystem::path& path,
    std::error_code& error) const {
    std::filesystem::remove_all(path, error);
    return !error;
}

OperationResult BackupService::createBackup(const Session& session) {
    if (!AccessController::isAuthorized(session, Operation::BackupData)) {
        return {false, ErrorCode::AccessDenied,
                "Only administrators can create backups."};
    }

    std::error_code error;
    std::filesystem::create_directories(dataDirectory_, error);

    if (error) {
        return {false, ErrorCode::StorageFailure,
                "Could not access the data directory."};
    }

    std::filesystem::create_directories(backupDirectory_, error);

    if (error) {
        return {false, ErrorCode::StorageFailure,
                "Could not create the backup directory."};
    }

    std::string backupName;
    std::filesystem::path backupPath;

    if (!createUniqueBackupDirectory(
            backupName, backupPath, error)) {
        return {false, ErrorCode::StorageFailure,
                "Could not create a unique backup folder."};
    }

    bool success =
        copyRegularFiles(dataDirectory_, backupPath, false, error);

    if (success) {
        success = validateBackup(backupPath);
    }

    if (!success) {
        std::error_code cleanupError;
        removeBackupDirectory(backupPath, cleanupError);

        if (auditService_) {
            auditService_->logBackup(
                session, backupName, false, "Backup creation failed.");
        }

        return {false, ErrorCode::StorageFailure,
                "Backup creation failed."};
    }

    if (auditService_) {
        auditService_->logBackup(
            session, backupName, true, "Backup created successfully.");
    }

    return {true, ErrorCode::None,
            "Backup created successfully: " + backupName};
}

bool BackupService::prepareRollback(
    const std::string& backupName,
    std::filesystem::path& rollbackPath,
    std::error_code& error) const {
    rollbackPath = backupDirectory_ / (backupName + "_rollback");

    if (std::filesystem::exists(rollbackPath, error) || error) {
        if (!error) {
            error = std::make_error_code(std::errc::file_exists);
        }
        return false;
    }

    if (!std::filesystem::create_directory(rollbackPath, error) || error) {
        return false;
    }

    if (copyRegularFiles(
            dataDirectory_, rollbackPath, true, error)) {
        return true;
    }

    std::error_code cleanupError;
    removeBackupDirectory(rollbackPath, cleanupError);
    return false;
}

bool BackupService::removeObsoleteFiles(
    const std::filesystem::path& backupPath,
    std::error_code& error) const {
    for (const auto& entry :
         std::filesystem::directory_iterator(dataDirectory_, error)) {
        if (error) {
            return false;
        }

        if (!entry.is_regular_file(error) || error) {
            if (error) {
                return false;
            }
            continue;
        }

        if (entry.path().filename() == "audit.log") {
            continue;
        }

        const auto backupFile =
            backupPath / entry.path().filename();

        const bool exists =
            std::filesystem::exists(backupFile, error);

        if (error) {
            return false;
        }

        if (!exists &&
            (!std::filesystem::remove(entry.path(), error) || error)) {
            return false;
        }
    }

    return true;
}

bool BackupService::restoreFiles(
    const std::filesystem::path& backupPath,
    std::error_code& error) const {
    return copyRegularFiles(
        backupPath, dataDirectory_, true, error);
}

bool BackupService::rollbackRestore(
    const std::filesystem::path& rollbackPath,
    std::error_code& error) const {
    for (const auto& entry :
         std::filesystem::directory_iterator(dataDirectory_, error)) {
        if (error) {
            return false;
        }

        if (!entry.is_regular_file(error) || error) {
            if (error) {
                return false;
            }
            continue;
        }

        if (entry.path().filename() == "audit.log") {
            continue;
        }

        if (!std::filesystem::remove(entry.path(), error) || error) {
            return false;
        }
    }

    return copyRegularFiles(
        rollbackPath, dataDirectory_, true, error);
}

OperationResult BackupService::restoreBackup(
    const Session& session,
    const std::string& backupName,
    const std::string& confirmation) {
    if (!AccessController::isAuthorized(session, Operation::RestoreData)) {
        return {false, ErrorCode::AccessDenied,
                "Only administrators can restore backups."};
    }

    if (confirmation != "Y" && confirmation != "y") {
        return {false, ErrorCode::InvalidInput,
                "Restore cancelled. Confirmation was not provided."};
    }

    std::filesystem::path backupPath;

    if (!resolveBackupPath(backupName, backupPath)) {
        return {false, ErrorCode::InvalidInput,
                "Invalid backup name or backup path."};
    }

    if (!validateBackup(backupPath)) {
        return {false, ErrorCode::InvalidInput,
                "The selected backup is missing or invalid."};
    }

    std::error_code error;
    std::filesystem::create_directories(dataDirectory_, error);

    if (error) {
        return {false, ErrorCode::StorageFailure,
                "Could not access the data directory."};
    }

    std::filesystem::path rollbackPath;

    if (!prepareRollback(backupName, rollbackPath, error)) {
        return {false, ErrorCode::StorageFailure,
                "Could not prepare restore rollback."};
    }

    bool success = removeObsoleteFiles(backupPath, error);

    if (success) {
        success = restoreFiles(backupPath, error);
    }

    if (!success) {
        std::error_code rollbackError;
        const bool rollbackSucceeded =
            rollbackRestore(rollbackPath, rollbackError);

        if (rollbackSucceeded) {
            std::error_code cleanupError;
            removeBackupDirectory(rollbackPath, cleanupError);
        }

        if (auditService_) {
            auditService_->logRestore(
                session, backupName, false,
                rollbackSucceeded
                    ? "Restore failed; original data was recovered."
                    : "Restore and rollback failed; recovery data retained.");
        }

        return {false, ErrorCode::StorageFailure,
                rollbackSucceeded
                    ? "Restore failed. Original data was recovered."
                    : "Restore failed and rollback was unsuccessful. "
                      "Rollback data has been retained for recovery."};
    }

    std::error_code cleanupError;
    removeBackupDirectory(rollbackPath, cleanupError);

    if (auditService_) {
        auditService_->logRestore(
            session, backupName, true, "Data restored successfully.");
    }

    return {true, ErrorCode::None,
            "Backup restored successfully: " + backupName};
}

}  // namespace srm
