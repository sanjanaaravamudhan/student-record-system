
#include "srm/attendance_service.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

using srm::AttendanceService;
using srm::ErrorCode;
using srm::Role;
using srm::Session;

const Session teacher{Role::Teacher, true, {}};
const Session ownStudent{Role::Student, true, "S001"};
const Session otherStudent{Role::Student, true, "S002"};

std::filesystem::path writeAttendanceFile() {
    const auto path = std::filesystem::temp_directory_path() /
                      "srm-attendance.tsv";

    std::ofstream output(path, std::ios::trunc);

    // S001: Mathematics = 2/3 = 66.7%
    output << "S001\tMathematics\t2026-10-01\tP\n";
    output << "S001\tMathematics\t2026-10-02\tP\n";
    output << "S001\tMathematics\t2026-10-03\tA\n";

    // S002: Mathematics = 0/2 = 0%
    output << "S002\tMathematics\t2026-10-01\tA\n";
    output << "S002\tMathematics\t2026-10-02\tA\n";

    // S003: Mathematics = 8/10 = 80%
    output << "S003\tMathematics\t2026-10-01\tP\n";
    output << "S003\tMathematics\t2026-10-02\tP\n";
    output << "S003\tMathematics\t2026-10-03\tP\n";
    output << "S003\tMathematics\t2026-10-04\tP\n";
    output << "S003\tMathematics\t2026-10-05\tP\n";
    output << "S003\tMathematics\t2026-10-06\tP\n";
    output << "S003\tMathematics\t2026-10-07\tP\n";
    output << "S003\tMathematics\t2026-10-08\tP\n";
    output << "S003\tMathematics\t2026-10-09\tA\n";
    output << "S003\tMathematics\t2026-10-10\tA\n";

    output.close();
    return path;
}

AttendanceService serviceFor(const std::filesystem::path& path) {
    return AttendanceService(
        path,
        [](const std::string& rollNumber) {
            return rollNumber == "S001" ||
                   rollNumber == "S002" ||
                   rollNumber == "S003";
        });
}

void testTeacherCanViewAnyStudent() {
    const auto path = writeAttendanceFile();
    auto service = serviceFor(path);

    const auto report = service.generateReport(teacher, "S002");

    assert(report.success);
    assert(report.report.find("Attendance report for S002") !=
           std::string::npos);
    assert(report.report.find("BELOW 75%") != std::string::npos);

    std::filesystem::remove(path);
}

void testStudentCanViewOwnReport() {
    const auto path = writeAttendanceFile();
    auto service = serviceFor(path);

    const auto report = service.generateReport(ownStudent, "S001");

    assert(report.success);
    assert(report.report.find("Present 2/3") != std::string::npos);
    assert(report.report.find("66.7%") != std::string::npos);
    assert(report.report.find("BELOW 75%") != std::string::npos);

    std::filesystem::remove(path);
}

void testStudentCannotViewAnotherReport() {
    const auto path = writeAttendanceFile();
    auto service = serviceFor(path);

    const auto report = service.generateReport(otherStudent, "S001");

    assert(!report.success);
    assert(report.error == ErrorCode::AccessDenied);

    std::filesystem::remove(path);
}

void testSecureInputValidation() {
    const auto path = writeAttendanceFile();
    auto service = serviceFor(path);

    const std::string longInput(101, 'A');

    auto emptyReport = service.generateReport(teacher, "");
    assert(!emptyReport.success);
    assert(emptyReport.error == ErrorCode::InvalidInput);

    auto longReport = service.generateReport(teacher, longInput);
    assert(!longReport.success);
    assert(longReport.error == ErrorCode::InvalidInput);

    auto malformedReport = service.generateReport(teacher, "S001\n");
    assert(!malformedReport.success);
    assert(malformedReport.error == ErrorCode::InvalidInput);

    assert(!service.deleteStudentAttendance(""));
    assert(!service.deleteStudentAttendance(longInput));
    assert(!service.deleteStudentAttendance("S001\n"));

    const auto report = service.generateReport(teacher, "S001");
    assert(report.success);
    assert(report.report.find("Present 2/3") != std::string::npos);

    std::filesystem::remove(path);
}

void testAttendanceBelow75PercentIsFlagged() {
    const auto path = writeAttendanceFile();
    auto service = serviceFor(path);

    const auto report = service.generateReport(teacher, "S002");

    assert(report.success);
    assert(report.report.find("Present 0/2") != std::string::npos);
    assert(report.report.find("0.0%") != std::string::npos);
    assert(report.report.find("BELOW 75%") != std::string::npos);

    std::filesystem::remove(path);
}

void testAttendanceExactlyAt75PercentIsNotFlagged() {
    const auto path = std::filesystem::temp_directory_path() /
                      "srm-attendance-75-test.tsv";

    {
        std::ofstream output(path, std::ios::trunc);
        output << "S001\tMathematics\t2026-10-01\tP\n";
        output << "S001\tMathematics\t2026-10-02\tP\n";
        output << "S001\tMathematics\t2026-10-03\tP\n";
        output << "S001\tMathematics\t2026-10-04\tA\n";
    }

    auto service = serviceFor(path);
    const auto report = service.generateReport(ownStudent, "S001");

    assert(report.success);
    assert(report.report.find("75.0%") != std::string::npos);
    assert(report.report.find("BELOW 75%") == std::string::npos);

    std::filesystem::remove(path);
}

void testAttendanceAbove75PercentIsNotFlagged() {
    const auto path = writeAttendanceFile();
    auto service = serviceFor(path);

    const auto report = service.generateReport(teacher, "S003");

    assert(report.success);
    assert(report.report.find("Present 8/10") != std::string::npos);
    assert(report.report.find("80.0%") != std::string::npos);
    assert(report.report.find("BELOW 75%") == std::string::npos);

    std::filesystem::remove(path);
}

void testAttendanceIsCalculatedIndependentlyPerSubject() {
    const auto path = std::filesystem::temp_directory_path() /
                      "srm-attendance-subject-test.tsv";

    {
        std::ofstream output(path, std::ios::trunc);

        // S001 Mathematics: 2/3 = 66.7%
        output << "S001\tMathematics\t2026-10-01\tP\n";
        output << "S001\tMathematics\t2026-10-02\tP\n";
        output << "S001\tMathematics\t2026-10-03\tA\n";

        // S001 Physics: 4/5 = 80%
        output << "S001\tPhysics\t2026-10-01\tP\n";
        output << "S001\tPhysics\t2026-10-02\tP\n";
        output << "S001\tPhysics\t2026-10-03\tP\n";
        output << "S001\tPhysics\t2026-10-04\tP\n";
        output << "S001\tPhysics\t2026-10-05\tA\n";
    }

    auto service = serviceFor(path);
    const auto report = service.generateReport(ownStudent, "S001");

    assert(report.success);

    assert(report.report.find(
               "Mathematics | Present 2/3 | 66.7% | BELOW 75%") !=
           std::string::npos);

    assert(report.report.find(
               "Physics | Present 4/5 | 80.0%") !=
           std::string::npos);

    std::filesystem::remove(path);
}

}  // namespace

int main() {
    testTeacherCanViewAnyStudent();
    testStudentCanViewOwnReport();
    testStudentCannotViewAnotherReport();
    testSecureInputValidation();
    testAttendanceBelow75PercentIsFlagged();
    testAttendanceExactlyAt75PercentIsNotFlagged();
    testAttendanceAbove75PercentIsNotFlagged();
    testAttendanceIsCalculatedIndependentlyPerSubject();

    return 0;
}
