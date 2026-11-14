
#include "srm/backup_service.h"
#include "srm/common_types.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

void writeFile(const fs::path& path, const std::string& content) {
    std::ofstream file(path, std::ios::trunc);
    assert(file.is_open());
    file << content;
    assert(file.good());
}

std::string readFile(const fs::path& path) {
    std::ifstream file(path);
    return std::string(
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>());
}

fs::path createTestRoot(const std::string& name) {
    const auto uniqueId =
        std::chrono::steady_clock::now()
            .time_since_epoch().count();

    return fs::temp_directory_path() /
           ("srm_" + name + "_" + std::to_string(uniqueId));
}

srm::Session makeAdmin() {
    srm::Session admin;
    admin.role = srm::Role::Administrator;
    admin.authenticated = true;
    admin.username = "admin";
    return admin;
}

srm::Session makeStudent() {
    srm::Session student;
    student.role = srm::Role::Student;
    student.authenticated = true;
    student.username = "student";
    return student;
}

void testBackupCreationAndRestore() {
    const fs::path root = createTestRoot("backup_test");
    const fs::path dataDir = root / "data";
    const fs::path backupDir = root / "backups";

    fs::create_directories(dataDir);

    writeFile(dataDir / "students.txt", "STUDENT_OLD");
    writeFile(dataDir / "users.txt", "USER_OLD");

    const auto admin = makeAdmin();
    const auto student = makeStudent();

    srm::BackupService backupService(dataDir, backupDir);

    // Test 1: Administrator can create a backup.
    const auto backupResult = backupService.createBackup(admin);
    assert(backupResult.success);

    const auto backups = backupService.listBackups();
    assert(!backups.empty());

    const std::string backupName =
        backups.back().filename().string();

    // Test 2: Backup contains all original data files.
    assert(fs::exists(backups.back() / "students.txt"));
    assert(fs::exists(backups.back() / "users.txt"));

    assert(readFile(backups.back() / "students.txt") == "STUDENT_OLD");
    assert(readFile(backups.back() / "users.txt") == "USER_OLD");

    // Test 3: Non-administrators cannot create backups.
    const auto deniedBackup = backupService.createBackup(student);
    assert(!deniedBackup.success);
    assert(deniedBackup.error == srm::ErrorCode::AccessDenied);

    // Test 4: Restore without confirmation is rejected.
    const auto cancelledRestore =
        backupService.restoreBackup(admin, backupName, "N");

    assert(!cancelledRestore.success);
    assert(cancelledRestore.error == srm::ErrorCode::InvalidInput);

    // Test 5: Modify current data after creating the snapshot.
    writeFile(dataDir / "students.txt", "STUDENT_NEW");
    writeFile(dataDir / "extra.txt", "EXTRA_DATA");

    // Test 6: Administrator can restore the backup.
    const auto restoreResult =
        backupService.restoreBackup(admin, backupName, "Y");

    assert(restoreResult.success);

    // Test 7: Original data is restored.
    assert(readFile(dataDir / "students.txt") == "STUDENT_OLD");
    assert(readFile(dataDir / "users.txt") == "USER_OLD");

    // Test 8: Files created after the backup are removed.
    assert(!fs::exists(dataDir / "extra.txt"));

    // Test 9: Non-administrators cannot restore backups.
    const auto deniedRestore =
        backupService.restoreBackup(student, backupName, "Y");

    assert(!deniedRestore.success);
    assert(deniedRestore.error == srm::ErrorCode::AccessDenied);

    fs::remove_all(root);
}

void testBackupNamesAreValidated() {
    const fs::path root = createTestRoot("backup_names");
    const fs::path dataDir = root / "data";
    const fs::path backupDir = root / "backups";

    fs::create_directories(dataDir);
    writeFile(dataDir / "students.txt", "STUDENT_DATA");

    srm::BackupService service(dataDir, backupDir);
    const auto admin = makeAdmin();

    const auto backupResult = service.createBackup(admin);
    assert(backupResult.success);

    const auto backups = service.listBackups();
    assert(!backups.empty());

    const std::string validName =
        backups.front().filename().string();

    assert(service.restoreBackup(admin, ".", "Y").error ==
           srm::ErrorCode::InvalidInput);

    assert(service.restoreBackup(admin, "..", "Y").error ==
           srm::ErrorCode::InvalidInput);

    assert(service.restoreBackup(admin, "../students.txt", "Y").error ==
           srm::ErrorCode::InvalidInput);

    assert(service.restoreBackup(admin, "backup_invalid", "Y").error ==
           srm::ErrorCode::InvalidInput);

    // A genuine backup name remains valid.
    assert(service.restoreBackup(admin, validName, "Y").success);

    fs::remove_all(root);
}

void testBackupNamesAreUnique() {
    const fs::path root = createTestRoot("backup_unique");
    const fs::path dataDir = root / "data";
    const fs::path backupDir = root / "backups";

    fs::create_directories(dataDir);
    writeFile(dataDir / "students.txt", "STUDENT_DATA");

    srm::BackupService service(dataDir, backupDir);
    const auto admin = makeAdmin();

    // Create two backups in quick succession.
    const auto first = service.createBackup(admin);
    const auto second = service.createBackup(admin);

    assert(first.success);
    assert(second.success);

    const auto backups = service.listBackups();

    assert(backups.size() == 2);
    assert(backups[0].filename() != backups[1].filename());

    fs::remove_all(root);
}

void testRollbackRestoresOriginalDataAfterFailure() {
    const fs::path root = createTestRoot("backup_rollback");
    const fs::path dataDir = root / "data";
    const fs::path backupDir = root / "backups";

    fs::create_directories(dataDir);

    writeFile(dataDir / "students.txt", "STUDENT_OLD");
    writeFile(dataDir / "users.txt", "USER_OLD");

    fs::path selectedBackup;
    bool failSnapshotCopy = false;
    bool snapshotFailureUsed = false;

    srm::BackupService::CopyFileOperation copyOperation =
        [&](const fs::path& source,
            const fs::path& destination,
            std::error_code& error) {
            if (failSnapshotCopy &&
                !snapshotFailureUsed &&
                source.parent_path().filename() == selectedBackup.filename()) {
                snapshotFailureUsed = true;
                error = std::make_error_code(std::errc::io_error);
                return false;
            }

            fs::copy_file(
                source,
                destination,
                fs::copy_options::overwrite_existing,
                error);

            return !error;
        };

    srm::BackupService service(
        dataDir, backupDir, nullptr, copyOperation);

    const auto admin = makeAdmin();

    // Create a valid snapshot before enabling failure injection.
    const auto backupResult = service.createBackup(admin);
    assert(backupResult.success);

    const auto backups = service.listBackups();
    assert(!backups.empty());

    selectedBackup = backups.back();
    const std::string backupName =
        selectedBackup.filename().string();

    // Modify the live files.
    writeFile(dataDir / "students.txt", "STUDENT_NEW");
    writeFile(dataDir / "users.txt", "USER_NEW");
    writeFile(dataDir / "extra.txt", "EXTRA_DATA");

    // Force restoration from the selected backup to fail once.
    failSnapshotCopy = true;

    const auto result =
        service.restoreBackup(admin, backupName, "Y");

    assert(snapshotFailureUsed);
    assert(!result.success);
    assert(result.error == srm::ErrorCode::StorageFailure);

    // The rollback must recover the original live data.
    assert(readFile(dataDir / "students.txt") == "STUDENT_NEW");
    assert(readFile(dataDir / "users.txt") == "USER_NEW");
    assert(fs::exists(dataDir / "extra.txt"));

    // Successful recovery should clean up the rollback directory.
    assert(!fs::exists(backupDir / (backupName + "_rollback")));

    fs::remove_all(root);
}

void testRollbackDataIsRetainedWhenRecoveryFails() {
    const fs::path root = createTestRoot("backup_rollback_failure");
    const fs::path dataDir = root / "data";
    const fs::path backupDir = root / "backups";

    fs::create_directories(dataDir);

    writeFile(dataDir / "students.txt", "STUDENT_OLD");
    writeFile(dataDir / "users.txt", "USER_OLD");

    fs::path selectedBackup;
    bool failSnapshotCopy = false;
    bool failRollbackCopy = false;
    bool snapshotFailureUsed = false;
    bool rollbackFailureUsed = false;

    srm::BackupService::CopyFileOperation copyOperation =
        [&](const fs::path& source,
            const fs::path& destination,
            std::error_code& error) {
            const std::string sourceDirectory =
                source.parent_path().filename().string();

            if (failSnapshotCopy &&
                !snapshotFailureUsed &&
                source.parent_path().filename() == selectedBackup.filename()) {
                snapshotFailureUsed = true;
                error = std::make_error_code(std::errc::io_error);
                return false;
            }

            if (failRollbackCopy &&
                !rollbackFailureUsed &&
                sourceDirectory.find("_rollback") != std::string::npos) {
                rollbackFailureUsed = true;
                error = std::make_error_code(std::errc::io_error);
                return false;
            }

            fs::copy_file(
                source,
                destination,
                fs::copy_options::overwrite_existing,
                error);

            return !error;
        };

    srm::BackupService service(
        dataDir, backupDir, nullptr, copyOperation);

    const auto admin = makeAdmin();

    // Create a valid backup.
    const auto backupResult = service.createBackup(admin);
    assert(backupResult.success);

    const auto backups = service.listBackups();
    assert(!backups.empty());

    selectedBackup = backups.back();
    const std::string backupName =
        selectedBackup.filename().string();

    // Change live data after the backup.
    writeFile(dataDir / "students.txt", "STUDENT_NEW");
    writeFile(dataDir / "users.txt", "USER_NEW");

    failSnapshotCopy = true;
    failRollbackCopy = true;

    const auto result =
        service.restoreBackup(admin, backupName, "Y");

    assert(snapshotFailureUsed);
    assert(rollbackFailureUsed);
    assert(!result.success);
    assert(result.error == srm::ErrorCode::StorageFailure);

    // The rollback directory must remain after failed recovery.
    const fs::path rollbackPath =
        backupDir / (backupName + "_rollback");

    assert(fs::exists(rollbackPath));
    assert(fs::exists(rollbackPath / "students.txt"));
    assert(fs::exists(rollbackPath / "users.txt"));

    // The original pre-restoration contents must remain recoverable.
    assert(readFile(rollbackPath / "students.txt") == "STUDENT_NEW");
    assert(readFile(rollbackPath / "users.txt") == "USER_NEW");

    fs::remove_all(root);
}

int main() {
    testBackupCreationAndRestore();
    testBackupNamesAreValidated();
    testBackupNamesAreUnique();
    testRollbackRestoresOriginalDataAfterFailure();
    testRollbackDataIsRetainedWhenRecoveryFails();

    std::cout << "All backup service tests passed!\n";
    return 0;
}
