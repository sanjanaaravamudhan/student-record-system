
#include "srm/grade_service.h"

#include <cassert>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace {

using srm::ErrorCode;
using srm::GradeService;
using srm::Role;
using srm::Session;

const Session teacher{Role::Teacher, true, {}};
const Session student{Role::Student, true, "S001"};
const Session anotherStudent{Role::Student, true, "S002"};
const Session unauthenticatedStudent{Role::Student, false, "S001"};

GradeService serviceFor(const std::filesystem::path& path) {
    return GradeService(
        path,
        [](const std::string& rollNumber) { return rollNumber == "S001"; },
        [](const std::string& subject) { return subject == "Mathematics"; });
}

GradeService resultServiceFor(const std::filesystem::path& path) {
    return GradeService(
        path,
        [](const std::string& rollNumber) { return rollNumber == "S001"; },
        [](const std::string&) { return true; });
}

void testBoundaryMarksAreAccepted() {
    const auto path = std::filesystem::temp_directory_path() / "srm-grade-boundary.tsv";
    std::filesystem::remove(path);
    auto service = serviceFor(path);
    assert(service.enterOrUpdateMarks(teacher, "S001", "Mathematics", 1, "0").success);
    assert(service.enterOrUpdateMarks(teacher, "S001", "Mathematics", 2, "100").success);
    std::filesystem::remove(path);
}

void testInvalidMarksAreRejected() {
    const auto path = std::filesystem::temp_directory_path() / "srm-grade-invalid.tsv";
    std::filesystem::remove(path);
    auto service = serviceFor(path);
    for (const std::string input : {"-1", "101", "abc"}) {
        const auto result = service.enterOrUpdateMarks(
            teacher, "S001", "Mathematics", 1, input);
        assert(!result.success);
        assert(result.error == ErrorCode::InvalidInput);
    }
    std::filesystem::remove(path);
}

void testUpdateReplacesAndPersists() {
    const auto path = std::filesystem::temp_directory_path() / "srm-grade-update.tsv";
    std::filesystem::remove(path);
    {
        auto service = serviceFor(path);
        assert(service.enterOrUpdateMarks(teacher, "S001", "Mathematics", 1, "72").success);
        assert(service.enterOrUpdateMarks(teacher, "S001", "Mathematics", 1, "88").success);
        const auto report = service.generateReportCard(teacher, "S001");
        assert(report.success);
        assert(report.report.find("Marks 88") != std::string::npos);
        assert(report.report.find("Marks 72") == std::string::npos);
    }
    {
        auto reloaded = serviceFor(path);
        const auto report = reloaded.generateReportCard(teacher, "S001");
        assert(report.success);
        assert(report.report.find("Marks 88") != std::string::npos);
        assert(report.report.find("Marks 72") == std::string::npos);
    }
    std::filesystem::remove(path);
}

void testPermissionsAndIdentifiersAreValidated() {
    const auto path = std::filesystem::temp_directory_path() / "srm-grade-security.tsv";
    std::filesystem::remove(path);
    auto service = serviceFor(path);
    assert(service.enterOrUpdateMarks(student, "S001", "Mathematics", 1, "80").error ==
           ErrorCode::AccessDenied);
    assert(service.enterOrUpdateMarks(teacher, "S999", "Mathematics", 1, "80").error ==
           ErrorCode::StudentNotFound);
    assert(service.enterOrUpdateMarks(teacher, "S001", "Physics", 1, "80").error ==
           ErrorCode::SubjectNotFound);
    std::filesystem::remove(path);
}

void testSecureInputValidation() {
    const auto path = std::filesystem::temp_directory_path() / "srm-grade-input-security.tsv";
    std::filesystem::remove(path);

    auto service = serviceFor(path);

    // Empty and excessively long roll numbers are rejected.
    assert(service.enterOrUpdateMarks(
        teacher, "", "Mathematics", 1, "80").error == ErrorCode::InvalidInput);

    const std::string longInput(101, 'A');
    assert(service.enterOrUpdateMarks(
        teacher, longInput, "Mathematics", 1, "80").error == ErrorCode::InvalidInput);

    // Malformed text fields are rejected.
    assert(service.enterOrUpdateMarks(
        teacher, "S001", "Math\nematics", 1, "80").error == ErrorCode::InvalidInput);

    // Invalid semester values are rejected.
    assert(service.enterOrUpdateMarks(
        teacher, "S001", "Mathematics", 0, "80").error == ErrorCode::InvalidInput);

    assert(service.enterOrUpdateMarks(
        teacher, "S001", "Mathematics", -1, "80").error == ErrorCode::InvalidInput);

    // Decimal and malformed marks are rejected.
    assert(service.enterOrUpdateMarks(
        teacher, "S001", "Mathematics", 1, "12.5").error == ErrorCode::InvalidInput);

    assert(service.enterOrUpdateMarks(
        teacher, "S001", "Mathematics", 1, "12abc").error == ErrorCode::InvalidInput);

    // Invalid report-card roll numbers are rejected.
    const auto report = service.generateReportCard(teacher, "");
    assert(!report.success);
    assert(report.error == ErrorCode::InvalidInput);

    std::filesystem::remove(path);
}

void testGradeBoundaries() {
    const auto path = std::filesystem::temp_directory_path() / "srm-grade-scale.tsv";
    std::filesystem::remove(path);

    auto service = resultServiceFor(path);

    const std::vector<std::pair<int, std::string>> cases = {
        {100, "S"}, {90, "S"},
        {89, "A"}, {80, "A"},
        {79, "B"}, {70, "B"},
        {69, "C"}, {60, "C"},
        {59, "D"}, {50, "D"},
        {49, "E"}, {40, "E"},
        {39, "F"}, {0, "F"}
    };

    int subjectNumber = 1;

    for (const auto& testCase : cases) {
        const std::string subject = "Subject" + std::to_string(subjectNumber++);

        assert(service.enterOrUpdateMarks(
            teacher, "S001", subject, 1, std::to_string(testCase.first)).success);

        const auto report = service.generateReportCard(teacher, "S001");

        assert(report.success);

        const std::string expected =
            subject + " | Marks " + std::to_string(testCase.first) +
            " | Grade " + testCase.second;

        assert(report.report.find(expected) != std::string::npos);
    }

    std::filesystem::remove(path);
}

void testPercentageAndGPA() {
    const auto path = std::filesystem::temp_directory_path() / "srm-grade-calculation.tsv";
    std::filesystem::remove(path);

    auto service = resultServiceFor(path);

    assert(service.enterOrUpdateMarks(
        teacher, "S001", "Mathematics", 1, "80").success);

    assert(service.enterOrUpdateMarks(
        teacher, "S001", "Physics", 1, "70").success);

    assert(service.enterOrUpdateMarks(
        teacher, "S001", "Chemistry", 1, "90").success);

    const auto report = service.generateReportCard(teacher, "S001");

    assert(report.success);
    assert(report.report.find("Semester Percentage: 80.00%") != std::string::npos);
    assert(report.report.find("Semester Grade: A") != std::string::npos);
    assert(report.report.find("Semester GPA: 9.00") != std::string::npos);

    assert(report.report.find("Overall Percentage: 80.00%") != std::string::npos);
    assert(report.report.find("Overall Grade: A") != std::string::npos);
    assert(report.report.find("Overall GPA: 9.00") != std::string::npos);

    std::filesystem::remove(path);
}

void testMultipleSemesterCalculations() {
    const auto path = std::filesystem::temp_directory_path() / "srm-grade-semesters.tsv";
    std::filesystem::remove(path);

    auto service = resultServiceFor(path);

    assert(service.enterOrUpdateMarks(
        teacher, "S001", "Mathematics", 1, "80").success);
    assert(service.enterOrUpdateMarks(
        teacher, "S001", "Physics", 1, "70").success);

    assert(service.enterOrUpdateMarks(
        teacher, "S001", "Chemistry", 2, "90").success);
    assert(service.enterOrUpdateMarks(
        teacher, "S001", "Biology", 2, "60").success);

    const auto report = service.generateReportCard(teacher, "S001");

    assert(report.success);

    assert(report.report.find("Semester 1") != std::string::npos);
    assert(report.report.find("Semester 2") != std::string::npos);

    assert(report.report.find("Overall Percentage: 75.00%") != std::string::npos);
    assert(report.report.find("Overall Grade: B") != std::string::npos);
    assert(report.report.find("Overall GPA: 8.50") != std::string::npos);

    std::filesystem::remove(path);
}

void testStudentCanViewOwnReportCard() {
    const auto path = std::filesystem::temp_directory_path() /
                      "srm-own-report.tsv";
    std::filesystem::remove(path);

    auto service = serviceFor(path);

    const auto markResult = service.enterOrUpdateMarks(
        teacher, "S001", "Mathematics", 1, "85");
    assert(markResult.success);

    const auto report = service.generateReportCard(student, "S001");

    assert(report.success);
    assert(report.report.find("S001") != std::string::npos);
    assert(report.report.find("Marks 85") != std::string::npos);

    std::filesystem::remove(path);
}

void testStudentCannotViewAnotherStudentsReportCard() {
    const auto path = std::filesystem::temp_directory_path() /
                      "srm-other-student-report.tsv";
    std::filesystem::remove(path);

    auto service = serviceFor(path);

    const auto markResult = service.enterOrUpdateMarks(
        teacher, "S001", "Mathematics", 1, "85");
    assert(markResult.success);

    const auto report = service.generateReportCard(
        anotherStudent, "S001"
    );

    assert(!report.success);
    assert(report.error == ErrorCode::AccessDenied);

    std::filesystem::remove(path);
}

void testUnauthenticatedStudentCannotViewReportCard() {
    const auto path = std::filesystem::temp_directory_path() /
                      "srm-unauthenticated-report.tsv";
    std::filesystem::remove(path);

    auto service = serviceFor(path);

    const auto markResult = service.enterOrUpdateMarks(
        teacher, "S001", "Mathematics", 1, "85");
    assert(markResult.success);

    const auto report = service.generateReportCard(
        unauthenticatedStudent, "S001"
    );

    assert(!report.success);
    assert(report.error == ErrorCode::AccessDenied);

    std::filesystem::remove(path);
}

}  // namespace

int main() {
    testBoundaryMarksAreAccepted();
    testInvalidMarksAreRejected();
    testUpdateReplacesAndPersists();
    testPermissionsAndIdentifiersAreValidated();
    testSecureInputValidation();
    testGradeBoundaries();
    testPercentageAndGPA();
    testMultipleSemesterCalculations();
    testStudentCanViewOwnReportCard();
    testStudentCannotViewAnotherStudentsReportCard();
    testUnauthenticatedStudentCannotViewReportCard();
    return 0;
}
