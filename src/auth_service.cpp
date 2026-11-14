
#include "srm/auth_service.h"
#include "srm/audit_service.h"
#include "srm/storage_utils.h"
#include "srm/input_validator.h"

#include <chrono>
#include <fstream>
#include <sstream>
#include <utility>

namespace srm {

UserRepository::UserRepository(std::filesystem::path filePath)
    : filePath_(std::move(filePath)) {}

bool UserRepository::load() {
    recoverInterruptedSave(filePath_);
    users_.clear();

    std::ifstream input(filePath_);
    if (!input) {
        return !std::filesystem::exists(filePath_);
    }

    std::string line;
    while (std::getline(input, line)) {
        std::istringstream row(line);
        UserAccount user;
        std::string roleStr;
        std::string failedAttemptsStr;
        std::string isLockedStr;

        if (!std::getline(row, user.username, '\t') ||
            !std::getline(row, user.passwordHash, '\t') ||
            !std::getline(row, roleStr, '\t') ||
            !std::getline(row, failedAttemptsStr, '\t') ||
            !std::getline(row, isLockedStr, '\t') ||
            !stringToRole(roleStr, user.role) ||
            !InputValidator::parseInteger(
                failedAttemptsStr, user.failedAttempts)) {
            users_.clear();
            return false;
        }

        user.isLocked = (isLockedStr == "1");
        std::getline(row, user.studentRollNumber);
        users_.push_back(std::move(user));
    }

    return true;
}

bool UserRepository::save() const {
    return atomicWriteFile(filePath_, [&](std::ostream& output) {
        for (const UserAccount& user : users_) {
            output << user.username << '\t'
                   << user.passwordHash << '\t'
                   << roleToString(user.role) << '\t'
                   << user.failedAttempts << '\t'
                   << (user.isLocked ? "1" : "0") << '\t'
                   << user.studentRollNumber << '\n';
        }

        return output.good();
    });
}

bool UserRepository::addUser(UserAccount user) {
    if (findUser(user.username) != nullptr) {
        return false;
    }

    users_.push_back(std::move(user));

    if (!save()) {
        users_.pop_back();
        return false;
    }

    return true;
}

UserAccount* UserRepository::findUser(const std::string& username) {
    for (UserAccount& user : users_) {
        if (user.username == username) {
            return &user;
        }
    }

    return nullptr;
}

const UserAccount* UserRepository::findUser(
    const std::string& username) const {
    for (const UserAccount& user : users_) {
        if (user.username == username) {
            return &user;
        }
    }

    return nullptr;
}

const std::vector<UserAccount>& UserRepository::users() const {
    return users_;
}

AuthService::AuthService(
    std::filesystem::path userFile,
    unsigned int hashIterations)
    : repository_(std::move(userFile)),
      hashIterations_(hashIterations),
      storageReady_(repository_.load()) {}

OperationResult AuthService::setupInitialAdmin(
    const std::string& username,
    const std::string& password) {

    if (!storageReady_) {
        return {false, ErrorCode::StorageFailure,
                "User storage is not ready."};
    }

    if (!InputValidator::validateText(username) ||
        !InputValidator::validateText(password)) {
        return {false, ErrorCode::InvalidInput,
                "Username and password must be non-empty and valid."};
    }

    if (!repository_.users().empty()) {
        return {false, ErrorCode::AccessDenied,
                "Initial administrator setup is disabled because accounts already exist."};
    }

    std::string hashedPassword =
        Crypto::hashPassword(password, "", hashIterations_);

    UserAccount adminUser{
        username, std::move(hashedPassword),
        Role::Administrator, 0, false, ""
    };

    if (!repository_.addUser(std::move(adminUser))) {
        return {false, ErrorCode::StorageFailure,
                "Failed to persist initial administrator account."};
    }

    return {true, ErrorCode::None,
            "Initial administrator created successfully."};
}

OperationResult AuthService::registerUser(
    const Session& adminSession,
    const std::string& username,
    const std::string& password,
    Role role,
    const std::string& studentRollNumber) {

    const auto auth =
        AccessController::authorize(adminSession, Operation::RegisterUser);

    if (!auth.success) {
        return auth;
    }

    if (!storageReady_) {
        return {false, ErrorCode::StorageFailure,
                "User storage is not ready."};
    }

    if (!InputValidator::validateText(username) ||
        !InputValidator::validateText(password) ||
        (!studentRollNumber.empty() &&
         !InputValidator::validateText(studentRollNumber))) {
        return {false, ErrorCode::InvalidInput,
                "Username, password, or student roll number contains invalid input."};
    }

    if (repository_.findUser(username) != nullptr) {
        return {false, ErrorCode::DuplicateRecord,
                "User with this username already exists."};
    }

    std::string hashedPassword =
        Crypto::hashPassword(password, "", hashIterations_);

    UserAccount user{
        username, std::move(hashedPassword),
        role, 0, false, studentRollNumber
    };

    if (!repository_.addUser(std::move(user))) {
        return {false, ErrorCode::StorageFailure,
                "Failed to persist user to storage."};
    }

    if (auditService_) {
        auditService_->logOperation(
            adminSession, "REGISTER_USER", username, true,
            "Registered new " + roleToString(role) + " account");
    }

    return {true, ErrorCode::None,
            "User registered successfully."};
}

LoginResult AuthService::login(
    const std::string& username,
    const std::string& password) {

    if (!storageReady_) {
        return {false, ErrorCode::StorageFailure,
                "User storage is not ready.", {}};
    }

    if (!InputValidator::validateText(username) ||
        !InputValidator::validateText(password)) {
        return {false, ErrorCode::InvalidInput,
                "Username or password contains invalid input.", {}};
    }

    UserAccount* user = repository_.findUser(username);

    if (user == nullptr) {
        if (auditService_) {
            auditService_->logLogin(
                username, Role::Student, false, "Invalid username");
        }

        return {false, ErrorCode::InvalidCredentials,
                "Invalid username or password.", {}};
    }

    if (user->isLocked) {
        if (auditService_) {
            auditService_->logLogin(
                username, user->role, false,
                "Login rejected: Account is locked");
        }

        return {false, ErrorCode::AccountLocked,
                "Account is locked due to repeated failed login attempts. Contact an Administrator to unlock.",
                {}};
    }

    if (!Crypto::verifyPassword(password, user->passwordHash)) {
        int prevFailed = user->failedAttempts;
        bool prevLocked = user->isLocked;

        user->failedAttempts++;

        if (user->failedAttempts >= 3) {
            user->isLocked = true;
        }

        if (!repository_.save()) {
            user->failedAttempts = prevFailed;
            user->isLocked = prevLocked;

            return {false, ErrorCode::StorageFailure,
                    "Failed to persist login attempt state.", {}};
        }

        if (user->isLocked) {
            if (auditService_) {
                auditService_->logLogin(
                    username, user->role, false,
                    "Account locked after 3 consecutive failed login attempts");

                auditService_->logOperation(
                    Session{user->role, false,
                            user->studentRollNumber, username},
                    "ACCOUNT_LOCKED", username, true,
                    "Account locked due to 3 failed attempts");
            }

            return {false, ErrorCode::AccountLocked,
                    "Account locked after 3 consecutive failed login attempts.",
                    {}};
        }

        if (auditService_) {
            auditService_->logLogin(
                username, user->role, false,
                "Failed password attempt " +
                std::to_string(user->failedAttempts));
        }

        return {false, ErrorCode::InvalidCredentials,
                "Invalid username or password.", {}};
    }

    if (user->failedAttempts > 0) {
        int prevFailed = user->failedAttempts;
        user->failedAttempts = 0;

        if (!repository_.save()) {
            user->failedAttempts = prevFailed;

            return {false, ErrorCode::StorageFailure,
                    "Failed to persist login session state.", {}};
        }
    }

    if (auditService_) {
        auditService_->logLogin(
            username, user->role, true,
            "Successful authentication");
    }

    Session session{
        user->role, true, user->studentRollNumber,
        user->username, std::chrono::steady_clock::now()
    };

    return {true, ErrorCode::None,
            "Login successful.", session};
}

OperationResult AuthService::unlockAccount(
    const Session& adminSession,
    const std::string& username) {

    const auto auth =
        AccessController::authorize(adminSession, Operation::UnlockAccount);

    if (!auth.success) {
        return auth;
    }

    if (!storageReady_) {
        return {false, ErrorCode::StorageFailure,
                "User storage is not ready."};
    }

    if (!InputValidator::validateText(username)) {
        return {false, ErrorCode::InvalidInput,
                "Username contains invalid input."};
    }

    UserAccount* user = repository_.findUser(username);

    if (user == nullptr) {
        return {false, ErrorCode::StudentNotFound,
                "User account not found."};
    }

    bool prevLocked = user->isLocked;
    int prevFailed = user->failedAttempts;

    user->isLocked = false;
    user->failedAttempts = 0;

    if (!repository_.save()) {
        user->isLocked = prevLocked;
        user->failedAttempts = prevFailed;

        return {false, ErrorCode::StorageFailure,
                "Failed to save unlocked user state."};
    }

    if (auditService_) {
        auditService_->logOperation(
            adminSession, "ACCOUNT_UNLOCKED", username, true,
            "Administrator unlocked user account");
    }

    return {true, ErrorCode::None,
            "Account unlocked successfully."};
}

bool AuthService::validateNewPassword(
    const std::string& password) const {
    return InputValidator::validatePassword(password);
}

OperationResult AuthService::persistPasswordChange(
    UserAccount& user,
    const std::string& newPassword) {

    const std::string previousHash = user.passwordHash;

    user.passwordHash = Crypto::hashPassword(
        newPassword, "", hashIterations_);

    if (!repository_.save()) {
        user.passwordHash = previousHash;

        return {false, ErrorCode::StorageFailure,
                "Failed to save the new password."};
    }

    return {true, ErrorCode::None,
            "Password changed successfully."};
}

// SRM-F-004: Change the authenticated user's own password.
OperationResult AuthService::changePassword(
    const Session& session,
    const std::string& newPassword) {

    const auto auth = AccessController::authorize(
        session, Operation::ChangeOwnPassword);

    if (!auth.success) {
        return auth;
    }

    if (!storageReady_) {
        return {false, ErrorCode::StorageFailure,
                "User storage is not ready."};
    }

    if (!validateNewPassword(newPassword)) {
        return {false, ErrorCode::InvalidInput,
                "Password must contain at least 8 characters, "
                "at least one letter, and at least one digit."};
    }

    UserAccount* user = repository_.findUser(session.username);

    if (user == nullptr) {
        return {false, ErrorCode::InvalidCredentials,
                "Authenticated user account not found."};
    }

    const auto result = persistPasswordChange(*user, newPassword);

    if (!result.success) {
        return result;
    }

    if (auditService_) {
        auditService_->logOperation(
            session, "PASSWORD_CHANGED", session.username, true,
            "User changed their own password");
    }

    return result;
}

bool AuthService::isAccountLocked(
    const std::string& username) const {

    const auto* user = repository_.findUser(username);
    return user != nullptr && user->isLocked;
}

int AuthService::getFailedAttempts(
    const std::string& username) const {

    const auto* user = repository_.findUser(username);
    return user ? user->failedAttempts : 0;
}

void AuthService::setAuditService(
    AuditService* auditService,
    bool requireAudit) {

    auditService_ = auditService;
    requireAudit_ = requireAudit;
}

AuditService* AuthService::auditService() const {
    return auditService_;
}

bool AuthService::isAuditRequired() const {
    return requireAudit_;
}

}  // namespace srm
