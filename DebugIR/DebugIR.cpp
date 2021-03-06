//===--- DebugIR.cpp - Transform debug metadata to allow debugging IR -----===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// A Module transform pass that emits a succinct version of the IR and replaces
// the source file metadata to allow debuggers to step through the IR.
//
// FIXME: instead of replacing debug metadata, this pass should allow for
// additional metadata to be used to point capable debuggers to the IR file
// without destroying the mapping to the original source file.
//
//===----------------------------------------------------------------------===//

#include "llvm/IR/AssemblyAnnotationWriter.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/ValueMap.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Transforms/Instrumentation.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/InitializePasses.h"

#include "llvm/Transforms/IPO/PassManagerBuilder.h" // RegisterStandardPasses
#include "llvm/IR/LegacyPassManager.h"
#include <llvm/Support/CommandLine.h>

#include <string>

#include "DebugIR.h"

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

using namespace llvm;

#define DEBUG_TYPE "debug-ir"

static cl::opt<std::string> ClABIListFile(
    "debugir-abilist",
    cl::desc("File listing native ABI functions and how the pass treats them"),
		cl::init("debugir-abilist.txt"),
    cl::Hidden);

namespace {

/// Builds a map of Value* to line numbers on which the Value appears in a
/// textual representation of the IR by plugging into the AssemblyWriter by
/// masquerading as an AssemblyAnnotationWriter.
class ValueToLineMap : public AssemblyAnnotationWriter {
  ValueMap<const Value *, unsigned int> Lines;
  typedef ValueMap<const Value *, unsigned int>::const_iterator LineIter;

  void addEntry(const Value *V, formatted_raw_ostream &Out) {
    Out.flush();
    Lines.insert(std::make_pair(V, Out.getLine() + 1));
  }

public:
  /// Prints Module to a null buffer in order to build the map of Value pointers
  /// to line numbers.
  ValueToLineMap(const Module *M, ABIList *DebugIRABIList) {
    raw_null_ostream ThrowAway;
    for (const Function &F : *M) {
      if (!DebugIRABIList->isIn(F, "debugir"))
        continue;
      if (F.isDeclaration())
        continue;
      F.print(ThrowAway, this);
    }
    // M->print(ThrowAway, this);
  }

  // This function is called after an Instruction, GlobalValue, or GlobalAlias
  // is printed.
  void printInfoComment(const Value &V, formatted_raw_ostream &Out) override {
    addEntry(&V, Out);
  }

  void emitBasicBlockStartAnnot(const BasicBlock *B,
                                formatted_raw_ostream &Out) override {
    addEntry(B, Out);
  }

  void emitFunctionAnnot(const Function *F,
                         formatted_raw_ostream &Out) override {
    addEntry(F, Out);
  }

  /// If V appears on a line in the textual IR representation, sets Line to the
  /// line number and returns true, otherwise returns false.
  bool getLine(const Value *V, unsigned int &Line) const {
    LineIter i = Lines.find(V);
    if (i != Lines.end()) {
      Line = i->second;
      return true;
    }
    return false;
  }
};

/// Updates debug metadata in a Module:
///   - changes Filename/Directory to values provided on construction
///   - adds/updates line number (DebugLoc) entries associated with each
///     instruction to reflect the instruction's location in an LLVM IR file
class DIUpdater : public InstVisitor<DIUpdater> {
  /// Builder of debug information
  DIBuilder Builder;

  /// Helper for type attributes/sizes/etc
  DataLayout Layout;

  /// Map of Value* to line numbers
  const ValueToLineMap LineTable;

  /// Map of Value* (in original Module) to Value* (in optional cloned Module)
  const ValueToValueMapTy *VMap;

  /// Directory of debug metadata
  DebugInfoFinder Finder;

  /// Source filename and directory
  StringRef Filename;
  StringRef Directory;

  // CU nodes needed when creating DI subprograms
  DIFile *FileNode;
  DILexicalBlockFile *LexicalBlockFileNode;

  Module &M;

  // CU nodes needed when creating DI subprograms
  // DIFile *File;
  // DIScope *Scope;

  ValueMap<const Function *, DISubprogram *> SubprogramDescriptors;
  DenseMap<const Type *, DIType *> TypeDescriptors;

	ABIList *DebugIRABIList;

public:
  DIUpdater(Module &M, StringRef Filename = StringRef(),
            StringRef Directory = StringRef(),
						ABIList *DebugIRABIList = nullptr, const Module *DisplayM = nullptr,
            const ValueToValueMapTy *VMap = nullptr)
      : Builder(M), Layout(&M), LineTable(DisplayM ? DisplayM : &M, DebugIRABIList),
				DebugIRABIList(DebugIRABIList), VMap(VMap),
        Finder(), Filename(Filename), Directory(Directory), FileNode(nullptr),
        LexicalBlockFileNode(nullptr), M(M) {

    // Even without finder, this screws up.
    // Finder.processModule(M);
    visit(&M);
  }

  ~DIUpdater() { Builder.finalize(); }

  void visitModule(Module &M) {
    // if (Finder.compile_unit_count() > 1)
    //   report_fatal_error("DebugIR pass supports only a signle compile unit per "
    //                      "Module.");
    // createCompileUnit(Finder.compile_unit_count() == 1
    //                       ? (DICompileUnit *)*Finder.compile_units().begin()
    //                       : nullptr);
    createCompileUnit(nullptr);
  }

  void visitFunction(Function &F) {
    if (!DebugIRABIList->isIn(F, "debugir"))
      return;
    if (F.isDeclaration() || findDISubprogram(&F))
      return;

    // remove old debug symbols
    stripDebugInfo(F);

    StringRef MangledName = F.getName();
    DISubroutineType *Sig = createFunctionSignature(&F);

    // find line of function declaration
    unsigned Line = 0;
    if (!findLine(&F, Line)) {
      LLVM_DEBUG(dbgs() << "WARNING: No line for Function " << F.getName().str()
                        << "\n");
      return;
    }

    Instruction *FirstInst = &*F.begin()->begin();
    unsigned ScopeLine = 0;
    if (!findLine(FirstInst, ScopeLine)) {
      LLVM_DEBUG(dbgs() << "WARNING: No line for 1st Instruction in Function "
                        << F.getName().str() << "\n");
      return;
    }

    bool IsOptimized = false;

    DISubprogram::DISPFlags SPFlags = DISubprogram::SPFlagDefinition;
    if (IsOptimized)
      SPFlags |= DISubprogram::SPFlagOptimized;
    if (F.hasPrivateLinkage() || F.hasInternalLinkage())
      SPFlags |= DISubprogram::SPFlagLocalToUnit;
    if (F.isDeclaration())
      SPFlags |= DISubprogram::SPFlagDefinition;
    DISubprogram *Sub =
        Builder.createFunction(FileNode, F.getName(), MangledName, FileNode,
                               Line, Sig, ScopeLine, DINode::FlagZero, SPFlags);
    F.setSubprogram(Sub);
    LLVM_DEBUG(dbgs() << "create subprogram mdnode " << *Sub << ": "
                      << "\n");

    SubprogramDescriptors.insert(std::make_pair(&F, Sub));

#if 0
    // TODO: This doesn't seem to work. Debug location declarations
    // seem to work only on allocas and for no other values. Clang
    // also copies function arguments to allocas and sets debug locations
    // on these allocas.
    for (size_t I = 0; I < F.arg_size(); I++) {
      auto *Arg = F.getArg(I);
      if (Arg->getName().empty())
        continue;

      auto Scope = getBlockScope(Sub, &F.getEntryBlock());
      auto DILV = Builder.createParameterVariable(
          Scope, Arg->getName(), I+1, FileNode, Line,
          getOrCreateType(Arg->getType()));
      auto Loc = DebugLoc::get(Line, 0, Scope);
      Builder.insertDeclare(Arg, DILV, Builder.createExpression(), Loc.get(),
                            FirstInst);
    }
#endif
  }

void visitInstruction(Instruction &I) {
    Function &F = *I.getFunction();
    if (!DebugIRABIList->isIn(F, "debugir"))
      return;

    DebugLoc Loc(I.getDebugLoc());

    /// If a ValueToValueMap is provided, use it to get the real instruction as
    /// the line table was generated on a clone of the module on which we are
    /// operating.
    Value *RealInst = nullptr;
    if (VMap)
      RealInst = VMap->lookup(&I);

    if (!RealInst)
      RealInst = &I;

    unsigned Col = 0; // FIXME: support columns
    unsigned Line;
    if (!LineTable.getLine(RealInst, Line)) {
      // Instruction has no line, it may have been removed (in the module that
      // will be passed to the debugger) so there is nothing to do here.
      LLVM_DEBUG(dbgs() << "WARNING: no LineTable entry for instruction "
                        << RealInst << "\n");
      return;
    }

    DILocalScope *Scope;
    DILocation *InlinedAt;
    if (Loc) {
      Scope = llvm::cast<DILocalScope>(Loc.getScope());
      InlinedAt = Loc.getInlinedAt();
    } else if ((Scope = dyn_cast<DILocalScope>(findScope(&I)))) {
      InlinedAt = nullptr;
    } else {
      LLVM_DEBUG(dbgs() << "WARNING: no valid scope for instruction " << &I
                        << ". no DebugLoc will be present."
                        << "\n");
      return;
    }

    DebugLoc NewLoc =
        DebugLoc(DILocation::get(M.getContext(), Line, Col, Scope, InlinedAt));
    addDebugLocation(I, NewLoc);

    if (!I.getType()->isVoidTy() && !I.getName().empty()) {
      auto DILV = Builder.createAutoVariable(Scope, I.getName(), FileNode, Line,
                                             getOrCreateType(I.getType()));
      Builder.insertDeclare(&I, DILV, Builder.createExpression(), NewLoc.get(),
                            I.getParent());
    }
  }

private:
  void createCompileUnit(DICompileUnit *CUToReplace) {
    std::string Flags;
    bool IsOptimized = false;
    StringRef Producer;
    unsigned RuntimeVersion(0);
    StringRef SplitName;

    if (CUToReplace) {
      // save fields from existing CU to re-use in the new CU
      // unique_ptr<DICompileUnit> ExistingCU = CUToReplace->clone();
      Producer = CUToReplace->getProducer();
      IsOptimized = CUToReplace->isOptimized();
      Flags = CUToReplace->getFlags().str();
      RuntimeVersion = CUToReplace->getRuntimeVersion();
      SplitName = CUToReplace->getSplitDebugFilename();
    } else {
      Producer =
          "LLVM Version " STR(LLVM_VERSION_MAJOR) "." STR(LLVM_VERSION_MINOR);
    }

    FileNode = Builder.createFile(Filename, Directory);
    DICompileUnit *CU =
        Builder.createCompileUnit(dwarf::DW_LANG_C99, FileNode, Producer,
                                  IsOptimized, Flags, RuntimeVersion);

    NamedMDNode *NMD = M.getOrInsertNamedMetadata("llvm.dbg.cu");
    NMD->clearOperands();
    NMD->addOperand(CU);

    // if (CUToReplace) {
    //    errs() << "CUToReplace: " << *CUToReplace << "\n";
    //    errs() << "CU: " << *CU << "\n";
    //  // CUToReplace->replaceAllUsesWith(CU);
    //};

    // LexicalBlockFileNode = FileNode; // Builder.createLexicalBlockFile(CU,
    // FileNode);
  }

  /// Returns the MDNode* that represents the DI scope to associate with I
  DIScope *findScope(const Instruction *I) {
    const Function *F = I->getParent()->getParent();
    if (DISubprogram *ret = findDISubprogram(F))
      return ret;

    LLVM_DEBUG(dbgs() << "WARNING: Using fallback lexical block file scope "
                 << LexicalBlockFileNode << " as scope for instruction " << I
                 << "\n");
    return LexicalBlockFileNode;
  }

  /// Returns the MDNode* that is the descriptor for F
  DISubprogram *findDISubprogram(const Function *F) {
    typedef ValueMap<const Function *, DISubprogram *>::const_iterator
        FuncNodeIter;
    FuncNodeIter i = SubprogramDescriptors.find(F);
    if (i != SubprogramDescriptors.end())
      return i->second;

    // LLVM_DEBUG(dbgs() << "searching for DI scope node for Function " << F
    //              << " in a list of " << Finder.subprogram_count()
    //              << " subprogram nodes"
    //              << "\n");

    // // TODO: When would this even be reached?
    // for (DISubprogram *S : Finder.subprograms()) {
    //   // TODO: Is describes correct?
    //   if (S->describes(F)) {
    //     LLVM_DEBUG(dbgs() << "Found DISubprogram " << S << " for function "
    //                  << F->getName() << "\n");
    //     return S;
    //   }
    // }
    // LLVM_DEBUG(dbgs() << "unable to find DISubprogram node for function "
    //              << F->getName().str() << "\n");
    return nullptr;
  }

  /// Sets Line to the line number on which V appears and returns true. If a
  /// line location for V is not found, returns false.
  bool findLine(const Value *V, unsigned &Line) {
    if (LineTable.getLine(V, Line))
      return true;

    if (VMap) {
      Value *mapped = VMap->lookup(V);
      if (mapped && LineTable.getLine(mapped, Line))
        return true;
    }
    return false;
  }

  std::string getTypeName(Type *T) {
    std::string TypeName;
    raw_string_ostream TypeStream(TypeName);
    if (T)
      T->print(TypeStream);
    else
      TypeStream << "Printing <null> Type";
    TypeStream.flush();
    return TypeName;
  }

  /// Returns the MDNode that represents type T if it is already created, or 0
  /// if it is not.
  DIType *getType(const Type *T) {
    typedef DenseMap<const Type *, DIType *>::const_iterator TypeNodeIter;
    TypeNodeIter i = TypeDescriptors.find(T);
    if (i != TypeDescriptors.end())
      return i->second;
    return nullptr;
  }

  /// Returns a DebugInfo type from an LLVM type T.
  DIType *getOrCreateType(Type *T) {
    DIType *N = getType(T);
    if (N)
      return N;
    else if (T->isVoidTy())
      return Builder.createUnspecifiedType("void");
    else if (T->isStructTy()) {
      // NOTE: where does DINodeArray come from?
      DICompositeType *S = Builder.createStructType(
          LexicalBlockFileNode, T->getStructName(), FileNode,
          /*LineNumber=*/0, Layout.getTypeSizeInBits(T),
          Layout.getABITypeAlignment(T), /*DIFlags=*/llvm::DINode::FlagZero,
          /*DerivedFrom=*/nullptr, llvm::DINodeArray()); // filled in later
      N = S; // the Node _is_ the struct type.

      // N is added to the map (early) so that element search below can find it,
      // so as to avoid infinite recursion for structs that contain pointers to
      // their own type.
      TypeDescriptors[T] = N;

      SmallVector<Metadata *, 4>
          Elements; // unfortunately, SmallVector<Type *> does not decay to
                    // SmallVector<Metadata *>

      for (unsigned i = 0; i < T->getStructNumElements(); ++i)
        Elements.push_back(getOrCreateType(T->getStructElementType(i)));

      Builder.replaceArrays(S, Builder.getOrCreateArray(Elements));
    } else if (T->isPointerTy()) {
      Type *PointeeTy = T->getPointerElementType();
      if (!(N = getType(PointeeTy)))
        N = Builder.createPointerType(
            getOrCreateType(PointeeTy), Layout.getPointerTypeSizeInBits(T),
            Layout.getPrefTypeAlignment(T), /*DWARFAddressSpace=*/None,
            getTypeName(T));
    } else if (T->isArrayTy()) {
      // assert(false && "unimplemented arrayty lowering.");
      SmallVector<Metadata *, 4>
          Subscripts; // unfortunately, SmallVector<Type *> does not decay to
                      // SmallVector<Metadata *>

      Subscripts.push_back(
          Builder.getOrCreateSubrange(0, T->getArrayNumElements() - 1));

      N = Builder.createArrayType(Layout.getTypeSizeInBits(T),
                                  Layout.getPrefTypeAlignment(T),
                                  getOrCreateType(T->getArrayElementType()),
                                  Builder.getOrCreateArray(Subscripts));
    } else {
      // assert(false && "unimplemented lowering for other types.");
      int encoding = llvm::dwarf::DW_ATE_signed;
      if (T->isIntegerTy())
        encoding = llvm::dwarf::DW_ATE_unsigned;
      else if (T->isFloatingPointTy())
        encoding = llvm::dwarf::DW_ATE_float;

      N = Builder.createBasicType(getTypeName(T), T->getPrimitiveSizeInBits(),
                                  encoding);
    }
    TypeDescriptors[T] = N;
    return N;
  }

  /// Returns a DebugInfo type that represents a function signature for Func.
  DISubroutineType *createFunctionSignature(const Function *Func) {
    SmallVector<Metadata *, 4> Params; // SmallVector<DIType *> does not
                                       // auto-case to SmallVector<Metadata *>
    DIType *ReturnType = getOrCreateType(Func->getReturnType());
    Params.push_back(ReturnType);

    for (const Argument &Arg : Func->args()) {
      Type *T = Arg.getType();
      Params.push_back(getOrCreateType(T));
    }

    DITypeRefArray ParamArray = Builder.getOrCreateTypeArray(Params);
    return Builder.createSubroutineType(ParamArray);
  }

  /// Associates Instruction I with debug location Loc.
  void addDebugLocation(Instruction &I, DebugLoc Loc) {
    I.setDebugLoc(Loc);
    // MDNode *MD = Loc.getAsMDNode();
    // I.setMetadata(LLVMContext::MD_dbg, MD);
  }
};

/// Sets Filename/Directory from the Module identifier and returns true, or
/// false if source information is not present.
bool getSourceInfoFromModule(const Module &M, std::string &Directory,
                             std::string &Filename) {
  std::string PathStr(M.getModuleIdentifier());
  if (PathStr.length() == 0 || PathStr == "<stdin>")
    return false;

  Filename = sys::path::filename(PathStr).str();
  SmallVector<char, 16> Path(PathStr.begin(), PathStr.end());
  sys::path::remove_filename(Path);
  Directory = StringRef(Path.data(), Path.size()).str();
  return true;
}

// Sets Filename/Directory from debug information in M and returns true, or
// false if no debug information available, or cannot be parsed.
bool getSourceInfoFromDI(const Module &M, std::string &Directory,
                         std::string &Filename) {
  NamedMDNode *CUNode = M.getNamedMetadata("llvm.dbg.cu");
  if (!CUNode || CUNode->getNumOperands() == 0)
    return false;

  DICompileUnit *CU = cast<DICompileUnit>(CUNode->getOperand(0));

  // Verify no longer exists?
  // if (!CU->Verify())
  //  return false;

  Filename = CU->getFilename().str();
  Directory = CU->getDirectory().str();
  return true;
}

} // anonymous namespace

namespace llvm {

DebugIR::DebugIR(bool HideDebugIntrinsics, bool HideDebugMetadata,
                 llvm::StringRef Directory, llvm::StringRef Filename)
    : ModulePass(ID), WriteSourceToDisk(true),
      HideDebugIntrinsics(HideDebugIntrinsics),
      HideDebugMetadata(HideDebugMetadata), Directory(Directory),
      Filename(Filename), GeneratedPath(false), ParsedPath(false) {
  // initializeDebugIRPass(*PassRegistry::getPassRegistry());
}

/// Modify input in-place; do not generate additional files, and do not hide
/// any debug intrinsics/metadata that might be present.
DebugIR::DebugIR()
    : ModulePass(ID), WriteSourceToDisk(true), HideDebugIntrinsics(false),
      HideDebugMetadata(false), GeneratedPath(false), ParsedPath(false) {
  // initializeDebugIRPass(*PassRegistry::getPassRegistry());
}

bool DebugIR::getSourceInfo(const Module &M) {
  ParsedPath = getSourceInfoFromDI(M, Directory, Filename) ||
               getSourceInfoFromModule(M, Directory, Filename);
  return ParsedPath;
}

bool DebugIR::updateExtension(StringRef NewExtension) {
  size_t dot = Filename.find_last_of(".");
  if (dot == std::string::npos)
    return false;

  Filename.erase(dot);
  Filename += NewExtension.str();
  errs() << "updateExtension: " << Filename << "\n";
  return true;
}

void DebugIR::generateFilename(std::unique_ptr<int> &fd) {
  SmallVector<char, 16> PathVec;
  fd.reset(new int);
  sys::fs::createTemporaryFile("debug-ir", "ll", *fd, PathVec);
  StringRef Path(PathVec.data(), PathVec.size());
  Filename = sys::path::filename(Path).str();
  sys::path::remove_filename(PathVec);
  Directory = StringRef(PathVec.data(), PathVec.size()).str();

  GeneratedPath = true;
}

std::string DebugIR::getPath() {
  SmallVector<char, 16> Path;
  sys::path::append(Path, Directory, Filename);
  Path.resize(Filename.size() + Directory.size() + 2);
  Path[Filename.size() + Directory.size() + 1] = '\0';
  return std::string(Path.data());
}

void DebugIR::writeDebugBitcode(const Module *M, int *fd) {
  std::unique_ptr<raw_fd_ostream> Out;
  std::error_code EC;

  if (!fd) {
    std::string Path = getPath();
    Out.reset(new raw_fd_ostream(Path, EC, sys::fs::F_Text));
    LLVM_DEBUG(dbgs() << "WRITING debug bitcode from Module " << M << " to file "
                 << Path << "\n");
  } else {
    LLVM_DEBUG(dbgs() << "WRITING debug bitcode from Module " << M << " to fd "
                 << *fd << "\n");
    Out.reset(new raw_fd_ostream(*fd, true));
  }

  for (const Function &F : *M) {
    if (!DebugIRABIList.isIn(F, "debugir"))
      continue;
    if (F.isDeclaration())
      continue;
    F.print(*Out, nullptr);
  }
  // M->print(*Out, nullptr);
  Out->close();
}

void DebugIR::createDebugInfo(Module &M, std::unique_ptr<Module> &DisplayM) {
  auto VMap = std::make_unique<ValueToValueMapTy>();
  DisplayM = CloneModule(M, *VMap);
  StripDebugInfo(*(DisplayM.get()));

  {
    // DIUpdater is in its own scope so that it's destructor, and hence
    // DIBuilder::finalize() gets called. Without that there's dangling stuff.
    DIUpdater R(M, Filename, Directory, &DebugIRABIList, DisplayM.get(), VMap.get());
  }

  auto DIVersionKey = "Debug Info Version";
  if (!M.getModuleFlag(DIVersionKey))
    // Add the current debug info version into the module.
    M.addModuleFlag(Module::Warning, DIVersionKey, DEBUG_METADATA_VERSION);

  // assert(!verifyModule(M, &errs()) && "verifyModule found issues");

  // if (M.getFunctionList().size() == 0)
  //   // no functions -- no debug info needed
  //   return;

  // std::unique_ptr<ValueToValueMapTy> VMap;

  // if (WriteSourceToDisk && (HideDebugIntrinsics || HideDebugMetadata)) {
  //   VMap.reset(new ValueToValueMapTy);
  //   DisplayM = CloneModule(M, *VMap);

  //   if (HideDebugIntrinsics)
  //     DebugIntrinsicsRemover::process(*DisplayM);

  //   if (HideDebugMetadata)
  //     DebugMetadataRemover::process(*DisplayM);
  // }

  // DIUpdater R(M, Filename, Directory, DisplayM.get(), VMap.get());
}

bool DebugIR::isMissingPath() { return Filename.empty() || Directory.empty(); }

bool DebugIR::runOnModule(Module &M) {
  errs() << "[DebugIR] start\n";
  std::unique_ptr<int> fd;

  std::vector<std::string> AllABIListFiles{ ClABIListFile };
  DebugIRABIList.set(
      SpecialCaseList::createOrDie(AllABIListFiles, *vfs::getRealFileSystem()));

  if (isMissingPath() && !getSourceInfo(M)) {
    if (!WriteSourceToDisk) {
      errs() << "DebugIR unable to determine file name in input. "
                "Ensure Module contains an identifier, a valid "
                "DICompileUnit, or construct DebugIR with "
                "non-empty Filename/Directory parameters.";
      return false;

    } else {
      generateFilename(fd);
    }
  }

  // assert(Filename != "");
  // assert(Directory != "");

  if (!GeneratedPath && WriteSourceToDisk)
    updateExtension(".debug-ll");

  dbgs() << "- Filename: " << Filename << " | Directory: " << Directory
               << "\n";

  // Clear line numbers. Keep debug info (if any) if we were able to read the
  // file name from the DICompileUnit descriptor.
  // DebugMetadataRemover::process(M, !ParsedPath);

  // Add the current debug info version into the module.
  // M.addModuleFlag(Module::Warning, "Debug Info Version",
  //                 DEBUG_METADATA_VERSION);
  auto DIVersionKey = "Debug Info Version";
  if (!M.getModuleFlag(DIVersionKey))
    // Add the current debug info version into the module.
    M.addModuleFlag(Module::Warning, DIVersionKey, DEBUG_METADATA_VERSION);

  std::unique_ptr<Module> DisplayM;
  createDebugInfo(M, DisplayM);

  if (WriteSourceToDisk) {
    Module *OutputM = DisplayM.get() ? DisplayM.get() : &M;
    writeDebugBitcode(OutputM, fd.get());
  }
  errs() << "[DebugIR] done\n";

  return true;
}

bool DebugIR::runOnModule(Module &M, std::string &Path) {
  bool result = runOnModule(M);
  Path = getPath();
  return result;
}

char DebugIR::ID = 0;

} // namespace llvm

static RegisterPass<DebugIR>
    X("debugir", "legacy-debugir",
      false,
      false
    );

static llvm::RegisterStandardPasses RegisterDebugIRLTOThinPass(
    PassManagerBuilder::EP_ModuleOptimizerEarly,
    [](const llvm::PassManagerBuilder &Builder,
       llvm::legacy::PassManagerBase &PM) { PM.add(new DebugIR()); });

static llvm::RegisterStandardPasses RegisterDebugIRLTOPass(
    llvm::PassManagerBuilder::EP_FullLinkTimeOptimizationEarly,
    [](const llvm::PassManagerBuilder &Builder,
       llvm::legacy::PassManagerBase &PM) { PM.add(new DebugIR()); });
