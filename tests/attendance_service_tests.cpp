
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
    const auto path = std::filesystem::temp_directory_path() / "srm-attendance.tsv";
    std::ofstream output(path, std::ios::trunc);
    output << "S001\tMathematics\t2026-10-01\tP\n";
    output << "S001\tMathematics\t2026-10-02\tP\n";
    output << "S001\tMathematics\t2026-10-03\tA\n";
    output << "S002\tMathematics\t2026-10-01\tA\n";
    output << "S002\tMathematics\t2026-10-02\tA\n";
    output.close();
    return path;
}

AttendanceService serviceFor(const std::filesystem::path& path) {
    return AttendanceService(
        path,
        [](const std::string& rollNumber) {
            return rollNumber == "S001" || rollNumber == "S002";
        });
}

void testTeacherCanViewAnyStudent() {
    const auto path = writeAttendanceFile();
    auto service = serviceFor(path);
    const auto report = service.generateReport(teacher, "S002");
    assert(report.success);
    assert(report.report.find("Attendance report for S002") != std::string::npos);
    assert(report.report.find("100.0%") == std::string::npos);
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

    // Empty roll number is rejected.
    auto emptyReport = service.generateReport(teacher, "");
    assert(!emptyReport.success);
    assert(emptyReport.error == ErrorCode::InvalidInput);

    // Excessively long roll number is rejected.
    auto longReport = service.generateReport(teacher, longInput);
    assert(!longReport.success);
    assert(longReport.error == ErrorCode::InvalidInput);

    // Newline in roll number is rejected.
    auto malformedReport = service.generateReport(teacher, "S001\n");
    assert(!malformedReport.success);
    assert(malformedReport.error == ErrorCode::InvalidInput);

    // Invalid roll numbers are rejected during deletion.
    assert(!service.deleteStudentAttendance(""));
    assert(!service.deleteStudentAttendance(longInput));
    assert(!service.deleteStudentAttendance("S001\n"));

    // Existing attendance records must remain unchanged.
    const auto report = service.generateReport(teacher, "S001");
    assert(report.success);
    assert(report.report.find("Present 2/3") != std::string::npos);

    std::filesystem::remove(path);
}

void testAttendanceDeletionPersistsAcrossRestart() {
    const auto path = std::filesystem::temp_directory_path() /
                      "srm-attendance-restart.tsv";

    std::filesystem::remove(path);

    // First, prepare attendance records as if they already exist.
    {
        std::ofstream output(path);
        output << "S001\tMathematics\t2026-10-01\tP\n";
        output << "S002\tMathematics\t2026-10-01\tA\n";
    }

    // First run: load records and delete S001's attendance.
    {
        srm::AttendanceRepository repository(path);

        const bool loaded = repository.load();
        assert(loaded);
        assert(repository.records().size() == 2);

        const bool removed = repository.removeByRollNumber("S001");
        assert(removed);
    }

    // Second run: verify the deletion was saved.
    {
        srm::AttendanceRepository restartedRepository(path);

        const bool reloaded = restartedRepository.load();
        assert(reloaded);
        assert(restartedRepository.records().size() == 1);
        assert(restartedRepository.records()[0].rollNumber == "S002");
    }

    std::filesystem::remove(path);
}

}  // namespace

int main() {
    testTeacherCanViewAnyStudent();
    testStudentCanViewOwnReport();
    testStudentCannotViewAnotherReport();
    testSecureInputValidation();
    testAttendanceDeletionPersistsAcrossRestart();
    return 0;
}
