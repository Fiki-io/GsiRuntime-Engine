#pragma once

#include <string>
#include <vector>

namespace gsi {

struct TestCaseResult {
    std::string name;
    bool passed = false;
    std::string details;
};

struct TestSuiteReport {
    int totalTests = 0;
    int passedTests = 0;
    std::vector<TestCaseResult> results;
    std::string formattedSummary;
};

class InitRunner {
public:
    static InitRunner& getInstance();

    TestSuiteReport runGsiTestSuite(const std::string& sandboxDir, bool hasExt4Vfs);

private:
    InitRunner() = default;
    ~InitRunner() = default;

    TestCaseResult testSandboxHierarchy(const std::string& sandboxDir);
    TestCaseResult testHookLibrary(const std::string& sandboxDir);
    TestCaseResult testExtractedBinaries(const std::string& sandboxDir);
    TestCaseResult testPropertyService(const std::string& sandboxDir);
    TestCaseResult testVirtualVfs(bool hasExt4Vfs);
    TestCaseResult testCommandExecution(const std::string& sandboxDir);
    TestCaseResult testInitServices(const std::string& sandboxDir);
};

} // namespace gsi
