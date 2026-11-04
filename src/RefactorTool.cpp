#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Refactoring.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"

#include <unordered_set>

#include "RefactorTool.h"

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::tooling;

static llvm::cl::OptionCategory ToolCategory("refactor-tool options");

// хранение базовых классов, с наследниками
static std::unordered_set<const clang::CXXRecordDecl *> ClassesWithDerived;

// Оптимиз-ный HandleTranslationUnit
void ComplexConsumer::HandleTranslationUnit(ASTContext &Context) {
    // Очищаем множество перед обработкой нового файла
    ClassesWithDerived.clear();

    // Однократный тяжелый проход по всему Translation Unit для поиска связей наследников
    for (const auto *D : Context.getTranslationUnitDecl()->decls()) {
        if (const auto *RD = dyn_cast<CXXRecordDecl>(D)) {
            // Если класс определен и у него есть базовые классы
            if (RD->hasDefinition() && RD->getNumBases() > 0) {
                // Идем по его базовым классам и запоминаем их
                for (const auto &Base : RD->bases()) {
                    if (const auto *BaseRecord = Base.getType()->getAsCXXRecordDecl()) {
                        if (BaseRecord->hasDefinition()) {
                            // Сохраняем указатель на базовый класс
                            ClassesWithDerived.insert(BaseRecord->getCanonicalDecl());
                        }
                    }
                }
            }
        }
    }

    Finder.matchAST(Context);
}

void RefactorHandler::run(const MatchFinder::MatchResult &Result) {
    auto &Diag = Result.Context->getDiagnostics();
    auto &SM = *Result.SourceManager;

    if (const auto *Dtor = Result.Nodes.getNodeAs<CXXDestructorDecl>("classDecl")) {
        if (SM.isInMainFile(Dtor->getLocation())) {
            const CXXRecordDecl *Record = Result.Nodes.getNodeAs<CXXRecordDecl>("baseClass");

            // Вместо тяжелого цикла — мгновенный поиск O(1) в хэш-таблице!
            if (Record && ClassesWithDerived.count(Record->getCanonicalDecl())) {
                handle_nv_dtor(Dtor, Diag, SM);
            }
        }
    }

    // Код для методов (override) и range-for остается без изменений
    if (const auto *Method = Result.Nodes.getNodeAs<CXXMethodDecl>("methodDecl");
        Method && Method->size_overridden_methods() > 0 && !Method->hasAttr<OverrideAttr>()) {
        if (SM.isInMainFile(Method->getLocation())) {
            handle_miss_override(Method, Diag, SM);
        }
    }

    if (const auto *LoopVar = Result.Nodes.getNodeAs<VarDecl>("VarDecl")) {
        if (SM.isInMainFile(LoopVar->getLocation())) {
            handle_crange_for(LoopVar, Diag, SM);
        }
    }
}

// Метод run вызывается для каждого совпадения с матчем.п
auto NvDtorMatcher() {
    return cxxDestructorDecl(
               unless(isVirtual()),
               unless(isImplicit()),  // КРИТИЧЕСКИ ВАЖНО: Игнорируем деструкторы, созданные компилятором автоматичеси
               ofClass(cxxRecordDecl(hasDefinition()).bind("baseClass")))
        .bind("classDecl");
}

// todo: необходимо реализовать обработку случая невиртуального деструктора
void RefactorHandler::handle_nv_dtor(const CXXDestructorDecl *Dtor, DiagnosticsEngine &Diag, SourceManager &SM) {
    SourceLocation Loc = Dtor->getLocation();  // Указывает на начало деструктора (~Имя)

    // check if virtual alread added
    unsigned LocRaw = Loc.getRawEncoding();
    if (virtualDtorLocations.count(LocRaw))
        return;
    virtualDtorLocations.insert(LocRaw);

    const unsigned DiagID = Diag.getCustomDiagID(
        DiagnosticsEngine::Warning, "Base class has childrenn, its dtor is non-virtual, adding 'virtual' ");
    Diag.Report(Loc, DiagID);
    Rewrite.InsertTextBefore(Loc, "virtual ");
}

// todo: необходимо реализовать обработку случая отсутствие override
void RefactorHandler::handle_miss_override(const CXXMethodDecl *Method, DiagnosticsEngine &Diag, SourceManager &SM) {
    SourceLocation Loc = Method->getLocation();

    const unsigned DiagID = Diag.getCustomDiagID(DiagnosticsEngine::Warning, "Method w/o override, addigg 'override'");
    Diag.Report(Loc, DiagID);

    // find )
    SourceLocation ParamEnd = Method->getParametersSourceRange().getEnd();
    SourceLocation RParenLoc =
        Lexer::findLocationAfterToken(ParamEnd, tok::r_paren, SM, Method->getASTContext().getLangOpts(), false);

    if (RParenLoc.isValid()) {
        Rewrite.InsertTextBefore(RParenLoc, " override");
    } else {
        SourceLocation InsertLoc = Method->hasBody() ? Method->getBody()->getBeginLoc() : Method->getEndLoc();
        Rewrite.InsertTextBefore(InsertLoc, " override ");
    }
}

// todo: необходимо реализовать обработку случая отсутствие & в range-for
void RefactorHandler::handle_crange_for(const VarDecl *LoopVar, DiagnosticsEngine &Diag, SourceManager &SM) {
    SourceLocation Loc = LoopVar->getLocation();

    TypeSourceInfo *TSI = LoopVar->getTypeSourceInfo();
    if (!TSI)
        return;

    SourceLocation TypeEndLoc = TSI->getTypeLoc().getEndLoc();
    SourceLocation InsertLoc = Lexer::getLocForEndOfToken(TypeEndLoc, 0, SM, LoopVar->getASTContext().getLangOpts());
    if (InsertLoc.isValid()) {
        const unsigned DiagID =
            Diag.getCustomDiagID(DiagnosticsEngine::Warning, "range-for copies const object, adding '&'");
        Diag.Report(Loc, DiagID);
        Rewrite.InsertTextBefore(InsertLoc, "&");
    }
}

// todo: ниже необходимо реализовать матчеры для поиска узлов AST
// note: синтаксис написания матчеров точно такой же как и для использования clang-query
/*
    Пример того, как может выглядеть реализация:
    auto AllClassesMatcher()
    {
        return cxxRecordDecl().bind("classDecl");
    }
*/

// Матчер для поиска переопределенных методов без override (Случай 2)
auto NoOverrideMatcher() {
    return cxxMethodDecl(isOverride(),
                         unless(cxxDestructorDecl()),  // Исключаем деструкторы
                         unless(isImplicit())          // Исключаем методы, сгенерированные компилятором
                         )
        .bind("methodDecl");
}

// Матчер для поиска range-for без ссылки (Случай 3)
auto NoRefConstVarInRangeLoopMatcher() {
    // Ищем переменную внутри cxxForRangeStmt, которая const, но не является ссылкой или фундаментальным типом
    return varDecl(hasType(isConstQualified()), unless(hasType(referenceType())),
                   unless(hasType(builtinType())),  // Исключаем int, char, double и т.д.
                   hasAncestor(cxxForRangeStmt()))
        .bind("VarDecl");
}

// Конструктор принимает Rewriter для изменения кода.
ComplexConsumer::ComplexConsumer(Rewriter &Rewrite) : Handler(Rewrite) {
    // Создаем MatchFinder и добавляем матчеры.
    Finder.addMatcher(NvDtorMatcher(), &Handler);
    Finder.addMatcher(NoOverrideMatcher(), &Handler);
    Finder.addMatcher(NoRefConstVarInRangeLoopMatcher(), &Handler);
}

std::unique_ptr<ASTConsumer> CodeRefactorAction::CreateASTConsumer(CompilerInstance &CI, StringRef file) {
    RewriterForCodeRefactor.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
    return std::make_unique<ComplexConsumer>(RewriterForCodeRefactor);
}

bool CodeRefactorAction::BeginSourceFileAction(CompilerInstance &CI) {
    // Инициализируем Rewriter для рефакторинга.
    RewriterForCodeRefactor.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
    return true;  // Возвращаем true, чтобы продолжить обработку файла.
}

void CodeRefactorAction::EndSourceFileAction() {
    // Применяем изменения в файле и сохраняем на диск
    if (RewriterForCodeRefactor.overwriteChangedFiles()) {
        llvm::errs() << "Error applying changes to files.\n";
    }
}

#ifndef WITHOUT_MAIN
int main(int argc, const char **argv) {
    // Парсер опций: Обрабатывает флаги командной строки, компиляционные базы данных.
    auto ExpectedParser = CommonOptionsParser::create(argc, argv, ToolCategory);
    if (!ExpectedParser) {
        llvm::errs() << ExpectedParser.takeError();
        return 1;
    }
    CommonOptionsParser &OptionsParser = ExpectedParser.get();
    // Создаем ClangTool
    ClangTool Tool(OptionsParser.getCompilations(), OptionsParser.getSourcePathList());
    // Запускаем RefactorAction.
    return Tool.run(newFrontendActionFactory<CodeRefactorAction>().get());
}
#endif