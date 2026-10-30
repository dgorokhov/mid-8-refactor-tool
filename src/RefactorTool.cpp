#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "clang/Tooling/Refactoring.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "llvm/Support/CommandLine.h"

#include <unordered_set>

#include "RefactorTool.h"

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::tooling;

static llvm::cl::OptionCategory ToolCategory("refactor-tool options");

// Метод run вызывается для каждого совпадения с матчем. 
// Мы проверяем тип совпадения по bind-именам и применяем рефакторинг.
void RefactorHandler::run(const MatchFinder::MatchResult &Result) {
    auto& Diag = Result.Context->getDiagnostics();
    auto& SM = *Result.SourceManager; // Получаем SourceManager для проверки isInMainFile
    
    if (const auto *Dtor = Result.Nodes.getNodeAs<CXXDestructorDecl>("classDecl")) {
            // есть ли у класса есть наследники в TU
            const CXXRecordDecl *Record = Dtor->getParent();
            bool hasDerived = false;
            for (const auto *D : Result.Context->getTranslationUnitDecl()->decls()) {
                if (const auto *RD = dyn_cast<CXXRecordDecl>(D)) {
                    if (RD->hasDefinition() && RD != Record && RD->isDerivedFrom(Record)) {
                        hasDerived = true;
                        break;
                    }
                }
            }
            if (hasDerived) {
                handle_nv_dtor(Dtor, Diag, SM);
            }
    }

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

//todo: необходимо реализовать обработку случая невиртуального деструктора
// Реализация случая 1: Добавление virtual к деструктору базового класса
void RefactorHandler::handle_nv_dtor(const CXXDestructorDecl *Dtor,
                            DiagnosticsEngine &Diag,
                            SourceManager &SM) {
    SourceLocation Loc = Dtor->getLocation(); // Указывает на начало деструктора (~Имя)
    
    // check if virtual alread added
    unsigned LocRaw = Loc.getRawEncoding();
    if (virtualDtorLocations.count(LocRaw)) return;
    virtualDtorLocations.insert(LocRaw);

    const unsigned DiagID = Diag.getCustomDiagID(
            DiagnosticsEngine::Warning,
            "Base class has childrenn, its dtor is non-virtual, adding 'virtual' "
        );
    Diag.Report(Loc, DiagID);
    Rewrite.InsertTextBefore(Loc, "virtual ");
}


//todo: необходимо реализовать обработку случая отсутствие override
void RefactorHandler::handle_miss_override(const CXXMethodDecl *Method,
                            DiagnosticsEngine &Diag,
                            SourceManager &SM) {
    SourceLocation Loc = Method->getLocation();

    const unsigned DiagID = Diag.getCustomDiagID(
            DiagnosticsEngine::Warning,
            "Method w/o override, addigg 'override'"
        );
    Diag.Report(Loc, DiagID);

    // find )
    SourceLocation ParamEnd = Method->getParametersSourceRange().getEnd();
    SourceLocation RParenLoc = Lexer::findLocationAfterToken(ParamEnd, tok::r_paren, SM, Method->getASTContext().getLangOpts(), false);

    if (RParenLoc.isValid()) {
        Rewrite.InsertTextBefore(RParenLoc, " override");
    } else {
        SourceLocation InsertLoc = Method->hasBody() ? Method->getBody()->getBeginLoc() : Method->getEndLoc();
        Rewrite.InsertTextBefore(InsertLoc, " override ");
    }
}

//todo: необходимо реализовать обработку случая отсутствие & в range-for
void RefactorHandler::handle_crange_for(const VarDecl *LoopVar,
                                        DiagnosticsEngine &Diag,
                                        SourceManager &SM){
    SourceLocation Loc = LoopVar->getLocation();

    const unsigned DiagID = Diag.getCustomDiagID(
            DiagnosticsEngine::Warning,
            "range-for copies const object, adding '&'"
        );
    Diag.Report(Loc, DiagID);
    TypeSourceInfo *TSI = LoopVar->getTypeSourceInfo();
    if (!TSI) return;
    SourceLocation TypeEndLoc = TSI->getTypeLoc().getEndLoc();
    SourceLocation InsertLoc = Lexer::getLocForEndOfToken(TypeEndLoc, 0, SM, LoopVar->getASTContext().getLangOpts());

    if (InsertLoc.isValid()) {
        Rewrite.InsertTextBefore(InsertLoc, "&");
    }
}



//todo: ниже необходимо реализовать матчеры для поиска узлов AST
//note: синтаксис написания матчеров точно такой же как и для использования clang-query
/*
    Пример того, как может выглядеть реализация:
    auto AllClassesMatcher()
    {
        return cxxRecordDecl().bind("classDecl");
    }
*/
auto NvDtorMatcher()
{
    return cxxDestructorDecl(
        unless(isVirtual()),
        ofClass(cxxRecordDecl(hasDefinition()))
    ).bind("classDecl");
}

// Матчер для поиска переопределенных методов без override (Случай 2)
auto NoOverrideMatcher()
{
    return cxxMethodDecl(
        isOverride(),
        unless(cxxDestructorDecl()), // Исключаем деструкторы
        unless(isImplicit())         // Исключаем методы, сгенерированные компилятором
    ).bind("methodDecl");
}

auto NoRefConstVarInRangeLoopMatcher()
{
    //todo: замените код ниже, на свою реализацию, необходимо реализовать матчеры для поиска range-for без &
    return varDecl().bind("VarDecl");
}

// Конструктор принимает Rewriter для изменения кода.
ComplexConsumer::ComplexConsumer(Rewriter &Rewrite) : Handler(Rewrite) {
    // Создаем MatchFinder и добавляем матчеры.
    Finder.addMatcher(NvDtorMatcher(), &Handler);
    Finder.addMatcher(NoOverrideMatcher(), &Handler);
    Finder.addMatcher(NoRefConstVarInRangeLoopMatcher(), &Handler);
}

// Метод HandleTranslationUnit вызывается для каждого файла.
void ComplexConsumer::HandleTranslationUnit(ASTContext &Context) {
    Finder.matchAST(Context);
}


std::unique_ptr<ASTConsumer> CodeRefactorAction::CreateASTConsumer(CompilerInstance &CI,
                                                StringRef file) {
    RewriterForCodeRefactor.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
    return std::make_unique<ComplexConsumer>(
        RewriterForCodeRefactor);
}

bool CodeRefactorAction::BeginSourceFileAction( CompilerInstance &CI) {
// Инициализируем Rewriter для рефакторинга.
RewriterForCodeRefactor.setSourceMgr(CI.getSourceManager(),
                                        CI.getLangOpts());
    return true;  // Возвращаем true, чтобы продолжить обработку файла.
}

void CodeRefactorAction::EndSourceFileAction() {
    // Применяем изменения в файле.
    if (RewriterForCodeRefactor.overwriteChangedFiles()) {
        llvm::errs() << "Error applying changes to files.\n";
    }
}


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