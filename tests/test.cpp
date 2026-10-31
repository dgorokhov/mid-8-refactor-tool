#include "../include/RefactorTool.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/Tooling.h"
#include <gtest/gtest.h>
#include <memory>
#include <string>
 
// Хелпер для запуска разработанного рефакторинга над строкой кода в памяти
std::string runRefactorOnCode(const std::string &Code) {
    struct TestAction : public clang::ASTFrontendAction {
        std::string &ResultOutput;
        TestAction(std::string &Out) : ResultOutput(Out) {}

        std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance &CI, clang::StringRef) override {
            R.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
            return std::make_unique<ComplexConsumer>(R);
        }
        void EndSourceFileAction() override {
            llvm::raw_string_ostream OS(ResultOutput);
            R.getEditBuffer(R.getSourceMgr().getMainFileID()).write(OS);
        }
        clang::Rewriter R;
    };

    std::string Output;
    auto Action = std::make_unique<TestAction>(Output);
    // Запускаем инструмент. Имя файла обязательно должно иметь расширение .cpp
    clang::tooling::runToolOnCode(std::move(Action), Code, "input_test_file.cpp");
    return Output.empty() ? Code : Output;
}

// === ТЕСТЫ СЛУЧАЯ 1: НЕВИРТУАЛЬНЫЕ ДЕСТРУКТОРЫ ===
TEST(RefactorTest, AddsVirtualToBaseDestructor) {
    std::string Input = "class Base { public: ~Base(); }; class Derived : public Base {};";
    std::string Expected = "class Base { public: virtual ~Base(); }; class Derived : public Base {};";
    EXPECT_EQ(runRefactorOnCode(Input), Expected);
}

TEST(RefactorTest, NoVirtualIfNoDerivedClasses) {
    std::string Input = "class Isolated { public: ~Isolated(); };";
    EXPECT_EQ(runRefactorOnCode(Input), Input);
}

// === ТЕСТЫ СЛУЧАЯ 2: ОТСУТСТВИЕ OVERRIDE ===
TEST(RefactorTest, AddsOverrideToDerivedMethod) {
    std::string Input = "class Base {\n"
                        "public:\n"
                        "    virtual void foo() {}\n"
                        "};\n"
                        "class Derived : public Base {\n"
                        "public:\n"
                        "    void foo() {}\n"
                        "};\n";

    std::string Output = runRefactorOnCode(Input);
    bool hasOverride = (Output.find("override") != std::string::npos);

    EXPECT_TRUE(hasOverride);
}

TEST(RefactorTest, NoOverrideIfAlreadyPresent) {
    std::string Input = "class Base { public: virtual ~Base() = default; virtual void foo() {} }; "
                        "class Derived : public Base { public: void foo() override {} };";
    EXPECT_EQ(runRefactorOnCode(Input), Input);
}

// === ТЕСТЫ СЛУЧАЯ 3: RANGE-FOR LOOP ===
TEST(RefactorTest, AddsReferenceToConstRangeForObj) {
    std::string Input = "namespace std { template<typename T> struct vector { T* begin(); T* end(); }; }\n"
                        "struct Big { char data[100]; };\n"
                        "void f(std::vector<Big>& v) { for (const Big x : v) {} }";
    std::string Expected = "namespace std { template<typename T> struct vector { T* begin(); T* end(); }; }\n"
                           "struct Big { char data[100]; };\n"
                           "void f(std::vector<Big>& v) { for (const Big& x : v) {} }";
    EXPECT_EQ(runRefactorOnCode(Input), Expected);
}

TEST(RefactorTest, NoReferenceForBuiltinTypes) {
    std::string Input = "namespace std { template<typename T> struct vector { T* begin(); T* end(); }; }\n"
                        "void f(std::vector<int>& v) { for (const int x : v) {} }";
    EXPECT_EQ(runRefactorOnCode(Input), Input);
}
