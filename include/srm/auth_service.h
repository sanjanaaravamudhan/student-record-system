
#pragma once

#include "srm/common_types.h"
#include "srm/crypto.h"
#include "srm/rbac.h"

#include <filesystem>
#include <string>
#include <vector>

namespace srm {

class AuditService;

struct UserAccount {
    std::string username;
    std::string passwordHash;
    Role role;
    int failedAttempts = 0;
    bool isLocked = false;
    std::string studentRollNumber = "";
};

struct LoginResult {
    bool success;
    ErrorCode error;
    std::string message;
    Session session;
};

class UserRepository {
public:
    explicit UserRepository(std::filesystem::path filePath);

    bool load();
    bool save() const;
    bool addUser(UserAccount user);
    UserAccount* findUser(const std::string& username);
    const UserAccount* findUser(const std::string& username) const;
    const std::vector<UserAccount>& users() const;

private:
    std::filesystem::path filePath_;
    std::vector<UserAccount> users_;
};

class AuthService {
public:
    explicit AuthService(std::filesystem::path userFile,
                         unsigned int hashIterations = 10000);

    OperationResult setupInitialAdmin(const std::string& username,
                                      const std::string& password);

    OperationResult registerUser(const Session& adminSession,
                                 const std::string& username,
                                 const std::string& password,
                                 Role role,
                                 const std::string& studentRollNumber = "");

    LoginResult login(const std::string& username,
                      const std::string& password);

    OperationResult unlockAccount(const Session& adminSession,
                                  const std::string& username);

    // SRM-F-004: Authenticated users can change their own passwords.
    OperationResult changePassword(const Session& session,
                                   const std::string& newPassword);

    bool isAccountLocked(const std::string& username) const;
    int getFailedAttempts(const std::string& username) const;

    void setAuditService(AuditService* auditService, bool requireAudit = false);
    AuditService* auditService() const;
    bool isAuditRequired() const;

private:
    bool validateNewPassword(const std::string& password) const;

    OperationResult persistPasswordChange(
        UserAccount& user,
        const std::string& newPassword);

    UserRepository repository_;
    unsigned int hashIterations_;
    bool storageReady_;
    AuditService* auditService_ = nullptr;
    bool requireAudit_ = false;
};

}  // namespace srm
