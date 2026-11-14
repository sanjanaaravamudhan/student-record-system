
#include "srm/auth_service.h"
#include "srm/crypto.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <chrono>
#include <iterator>

namespace {

using srm::AuthService;
using srm::Crypto;
using srm::ErrorCode;
using srm::Role;
using srm::Session;

const Session adminSession{Role::Administrator, true, {}, "admin"};
const Session teacherSession{Role::Teacher, true, {}, "teacher1"};
const Session studentSession{Role::Student, true, "S001", "student1"};

constexpr unsigned int TEST_ITERATIONS = 1000;

void testAccountLockoutAfterThreeFailedAttempts() {
    const auto path = std::filesystem::temp_directory_path() /
                      "srm-auth-lockout.tsv";
    std::filesystem::remove(path);

    AuthService service(path, TEST_ITERATIONS);

    const auto registrationResult = service.registerUser(
        adminSession, "john_doe", "Password123",
        Role::Student, "S001");
    assert(registrationResult.success);

    const auto res1 = service.login("john_doe", "WrongPass1");
    assert(!res1.success);
    assert(res1.error == ErrorCode::InvalidCredentials);
    assert(service.getFailedAttempts("john_doe") == 1);
    assert(!service.isAccountLocked("john_doe"));

    const auto res2 = service.login("john_doe", "WrongPass2");
    assert(!res2.success);
    assert(res2.error == ErrorCode::InvalidCredentials);
    assert(service.getFailedAttempts("john_doe") == 2);
    assert(!service.isAccountLocked("john_doe"));

    const auto res3 = service.login("john_doe", "WrongPass3");
    assert(!res3.success);
    assert(res3.error == ErrorCode::AccountLocked);
    assert(service.isAccountLocked("john_doe"));

    const auto res4 = service.login("john_doe", "Password123");
    assert(!res4.success);
    assert(res4.error == ErrorCode::AccountLocked);

    std::filesystem::remove(path);
}

void testLockoutPersistsAcrossRestart() {
    const auto path = std::filesystem::temp_directory_path() /
                      "srm-auth-restart.tsv";
    std::filesystem::remove(path);

    {
        AuthService service(path, TEST_ITERATIONS);

        const auto registrationResult = service.registerUser(
            adminSession, "jane_doe", "Secure456", Role::Teacher);
        assert(registrationResult.success);

        const auto attempt1 = service.login("jane_doe", "bad1");
        const auto attempt2 = service.login("jane_doe", "bad2");
        const auto attempt3 = service.login("jane_doe", "bad3");

        assert(!attempt1.success);
        assert(!attempt2.success);
        assert(!attempt3.success);
        assert(service.isAccountLocked("jane_doe"));
    }

    {
        AuthService restartedService(path, TEST_ITERATIONS);

        assert(restartedService.isAccountLocked("jane_doe"));

        const auto result = restartedService.login(
            "jane_doe", "Secure456");

        assert(!result.success);
        assert(result.error == ErrorCode::AccountLocked);
    }

    std::filesystem::remove(path);
}

void testAdminOnlyUnlock() {
    const auto path = std::filesystem::temp_directory_path() /
                      "srm-auth-unlock.tsv";
    std::filesystem::remove(path);

    AuthService service(path, TEST_ITERATIONS);

    const auto registrationResult = service.registerUser(
        adminSession, "bob", "BobPass99",
        Role::Student, "S002");
    assert(registrationResult.success);

    const auto attempt1 = service.login("bob", "x");
    const auto attempt2 = service.login("bob", "y");
    const auto attempt3 = service.login("bob", "z");

    assert(!attempt1.success);
    assert(!attempt2.success);
    assert(!attempt3.success);
    assert(service.isAccountLocked("bob"));

    const auto teacherUnlock = service.unlockAccount(
        teacherSession, "bob");

    assert(!teacherUnlock.success);
    assert(teacherUnlock.error == ErrorCode::AccessDenied);
    assert(service.isAccountLocked("bob"));

    const auto studentUnlock = service.unlockAccount(
        studentSession, "bob");

    assert(!studentUnlock.success);
    assert(studentUnlock.error == ErrorCode::AccessDenied);
    assert(service.isAccountLocked("bob"));

    const auto adminUnlock = service.unlockAccount(
        adminSession, "bob");

    assert(adminUnlock.success);
    assert(adminUnlock.error == ErrorCode::None);
    assert(!service.isAccountLocked("bob"));
    assert(service.getFailedAttempts("bob") == 0);

    const auto loginResult = service.login("bob", "BobPass99");

    assert(loginResult.success);
    assert(loginResult.session.authenticated);
    assert(loginResult.session.role == Role::Student);
    assert(loginResult.session.studentRollNumber == "S002");

    std::filesystem::remove(path);
}

void testSuccessfulLoginResetsFailedAttempts() {
    const auto path = std::filesystem::temp_directory_path() /
                      "srm-auth-reset.tsv";
    std::filesystem::remove(path);

    AuthService service(path, TEST_ITERATIONS);

    const auto registrationResult = service.registerUser(
        adminSession, "charlie", "Alpha123", Role::Teacher);
    assert(registrationResult.success);

    const auto failedLogin1 = service.login("charlie", "wrong1");
    const auto failedLogin2 = service.login("charlie", "wrong2");

    assert(!failedLogin1.success);
    assert(!failedLogin2.success);
    assert(service.getFailedAttempts("charlie") == 2);

    const auto successLogin = service.login(
        "charlie", "Alpha123");

    assert(successLogin.success);
    assert(service.getFailedAttempts("charlie") == 0);
    assert(!service.isAccountLocked("charlie"));

    std::filesystem::remove(path);
}

void testInitialAdminSetupAndUserRegistrationAuthorization() {
    const auto path = std::filesystem::temp_directory_path() /
                      "srm-auth-admin-setup.tsv";
    std::filesystem::remove(path);

    AuthService service(path, TEST_ITERATIONS);

    const auto setupRes = service.setupInitialAdmin(
        "superadmin", "RootPassword123");

    assert(setupRes.success);
    assert(setupRes.error == ErrorCode::None);

    const auto adminLogin = service.login(
        "superadmin", "RootPassword123");

    assert(adminLogin.success);
    assert(adminLogin.session.role == Role::Administrator);

    const auto secondSetup = service.setupInitialAdmin(
        "intruder", "HackerPass123");

    assert(!secondSetup.success);
    assert(secondSetup.error == ErrorCode::AccessDenied);

    const auto teacherRegister = service.registerUser(
        teacherSession, "new_student", "StudentPass1",
        Role::Student, "S301");

    assert(!teacherRegister.success);
    assert(teacherRegister.error == ErrorCode::AccessDenied);

    const auto studentRegister = service.registerUser(
        studentSession, "new_student", "StudentPass1",
        Role::Student, "S301");

    assert(!studentRegister.success);
    assert(studentRegister.error == ErrorCode::AccessDenied);

    const auto adminRegister = service.registerUser(
        adminSession, "new_student", "StudentPass1",
        Role::Student, "S301");

    assert(adminRegister.success);
    assert(adminRegister.error == ErrorCode::None);

    const auto dupRegister = service.registerUser(
        adminSession, "new_student", "AnotherPass2",
        Role::Student, "S302");

    assert(!dupRegister.success);
    assert(dupRegister.error == ErrorCode::DuplicateRecord);

    std::filesystem::remove(path);
}

void testPasswordHashingAndSecureStorage() {
    const auto path = std::filesystem::temp_directory_path() /
                      "srm-auth-crypto.tsv";
    std::filesystem::remove(path);

    AuthService service(path, TEST_ITERATIONS);
    const std::string rawPassword = "TopSecretPassword!#42";

    const auto registrationResult = service.registerUser(
        adminSession, "secure_user", rawPassword, Role::Teacher);
    assert(registrationResult.success);

    std::ifstream file(path);
    assert(file.is_open());

    std::string content(
        (std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>());

    file.close();

    assert(content.find(rawPassword) == std::string::npos);
    assert(content.find("pbkdf2_sha256$") != std::string::npos);

    const auto loginSuccess = service.login(
        "secure_user", rawPassword);

    assert(loginSuccess.success);
    assert(loginSuccess.session.role == Role::Teacher);

    const auto loginFail = service.login(
        "secure_user", "IncorrectPassword");

    assert(!loginFail.success);
    assert(loginFail.error == ErrorCode::InvalidCredentials);

    std::filesystem::remove(path);
}

void testCryptoPBKDF2Verification() {
    const std::string password = "TestPassword#2026";

    const std::string hash1 = Crypto::hashPassword(
        password, "", 500);

    const std::string hash2 = Crypto::hashPassword(
        password, "", 500);

    assert(hash1 != hash2);

    const bool verify1 = Crypto::verifyPassword(password, hash1);
    const bool verify2 = Crypto::verifyPassword(password, hash2);
    const bool verifyWrong = Crypto::verifyPassword(
        "WrongPassword", hash1);
    const bool verifyEmpty = Crypto::verifyPassword("", hash1);
    const bool verifyMalformed = Crypto::verifyPassword(
        password, "invalid_format_string");
    const bool verifyInvalidFormat = Crypto::verifyPassword(
        password, "sha256$100$salt$hash");

    assert(verify1);
    assert(verify2);
    assert(!verifyWrong);
    assert(!verifyEmpty);
    assert(!verifyMalformed);
    assert(!verifyInvalidFormat);
}

void testSessionTimeout() {
    Session session;
    session.authenticated = true;

    session.lastActivity =
        std::chrono::steady_clock::now() -
        std::chrono::minutes(6);

    assert(srm::isSessionExpired(session));

    session.lastActivity = std::chrono::steady_clock::now();

    assert(!srm::isSessionExpired(session));

    session.authenticated = false;

    assert(srm::isSessionExpired(session));
}

void testSecureInputValidation() {
    const auto path = std::filesystem::temp_directory_path() /
                      "srm-auth-input-validation.tsv";
    std::filesystem::remove(path);

    AuthService service(path, TEST_ITERATIONS);

    const std::string longInput(101, 'A');

    const auto emptyUsername =
        service.setupInitialAdmin("", "RootPassword123");

    assert(!emptyUsername.success);
    assert(emptyUsername.error == ErrorCode::InvalidInput);

    const auto longUsername =
        service.setupInitialAdmin(longInput, "RootPassword123");

    assert(!longUsername.success);
    assert(longUsername.error == ErrorCode::InvalidInput);

    const auto malformedUsername =
        service.setupInitialAdmin("admin\nuser", "RootPassword123");

    assert(!malformedUsername.success);
    assert(malformedUsername.error == ErrorCode::InvalidInput);

    const auto setup =
        service.setupInitialAdmin("admin", "RootPassword123");

    assert(setup.success);

    const auto emptyLogin =
        service.login("", "RootPassword123");

    assert(!emptyLogin.success);
    assert(emptyLogin.error == ErrorCode::InvalidInput);

    const auto longPassword =
        service.login("admin", longInput);

    assert(!longPassword.success);
    assert(longPassword.error == ErrorCode::InvalidInput);

    const auto invalidUnlock =
        service.unlockAccount(adminSession, "admin\n");

    assert(!invalidUnlock.success);
    assert(invalidUnlock.error == ErrorCode::InvalidInput);

    const auto invalidRoll = service.registerUser(
        adminSession, "student2", "StudentPass123",
        Role::Student, "S002\n");

    assert(!invalidRoll.success);
    assert(invalidRoll.error == ErrorCode::InvalidInput);

    const auto longRoll = service.registerUser(
        adminSession, "student3", "StudentPass123",
        Role::Student, longInput);

    assert(!longRoll.success);
    assert(longRoll.error == ErrorCode::InvalidInput);

    const auto validRegistration = service.registerUser(
        adminSession, "student4", "StudentPass123",
        Role::Student, "S004");

    assert(validRegistration.success);

    std::filesystem::remove(path);
}

void testPasswordChangePersistsAcrossRestart() {
    const auto path = std::filesystem::temp_directory_path() /
                      "srm-auth-password-change-restart.tsv";

    std::filesystem::remove(path);

    const std::string oldPassword = "OldPassword123";
    const std::string newPassword = "NewPassword456";

    // First run: register, log in, and change the password.
    {
        AuthService service(path, TEST_ITERATIONS);

        const auto registration = service.registerUser(
            adminSession,
            "password_restart_user",
            oldPassword,
            Role::Teacher);

        assert(registration.success);

        const auto login = service.login(
            "password_restart_user", oldPassword);

        assert(login.success);

        const auto changeResult = service.changePassword(
            login.session, newPassword);

        assert(changeResult.success);
    }

    // Second run: create a new service using the same storage file.
    {
        AuthService restartedService(path, TEST_ITERATIONS);

        const auto oldPasswordLogin = restartedService.login(
            "password_restart_user", oldPassword);

        assert(!oldPasswordLogin.success);
        assert(oldPasswordLogin.error == ErrorCode::InvalidCredentials);

        const auto newPasswordLogin = restartedService.login(
            "password_restart_user", newPassword);

        assert(newPasswordLogin.success);
        assert(newPasswordLogin.session.role == Role::Teacher);
    }

    std::filesystem::remove(path);
}

void testRegisteredUserPersistsAcrossRestart() {
    const auto path = std::filesystem::temp_directory_path() /
                      "srm-auth-user-restart.tsv";

    std::filesystem::remove(path);

    {
        AuthService service(path, TEST_ITERATIONS);

        const auto result = service.registerUser(
            adminSession,
            "persist_user",
            "Persist123",
            Role::Teacher);

        assert(result.success);
        assert(std::filesystem::exists(path));
    }

    {
        AuthService restartedService(path, TEST_ITERATIONS);

        const auto login = restartedService.login(
            "persist_user", "Persist123");

        assert(login.success);
        assert(login.session.role == Role::Teacher);
    }

    std::filesystem::remove(path);
}

}  // namespace

int main() {
    testAccountLockoutAfterThreeFailedAttempts();
    testLockoutPersistsAcrossRestart();
    testAdminOnlyUnlock();
    testSuccessfulLoginResetsFailedAttempts();
    testInitialAdminSetupAndUserRegistrationAuthorization();
    testPasswordHashingAndSecureStorage();
    testCryptoPBKDF2Verification();
    testSessionTimeout();
    testSecureInputValidation();
    testRegisteredUserPersistsAcrossRestart();
    testPasswordChangePersistsAcrossRestart();

    return 0;
}
