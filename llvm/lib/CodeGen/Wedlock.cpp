#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/IR/CFG.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

static cl::opt<bool> EnableWedlockPass("wedlock", cl::Hidden,
                                       cl::desc("Enable the wedlock pass"),
                                       cl::init(false));

static cl::opt<std::string> WedlockOutput("wedlock-output", cl::Hidden,
                                          cl::desc("The output filename"),
                                          cl::init("wedlock.jsonl"));

static cl::opt<bool>
    EnableMIPrettyPrinting("wedlock-pretty-print-mi", cl::Hidden,
                           cl::desc("Enable pretty-printing of MachineInstrs"),
                           cl::init(false));

static cl::opt<std::string>
    WedlockLoggingOutput("wedlock-logging-output", cl::Hidden,
                         cl::Optional, cl::desc("Logging and diagnostic output"));

namespace {

using JObject = json::Object;
using JArray = json::Array;
using JValue = json::Value;

struct WedlockPass : public MachineFunctionPass {
  static char ID;
  raw_fd_ostream *WedlockStream{nullptr};
  raw_fd_ostream *WedlockLoggingStream{nullptr};

  WedlockPass() : MachineFunctionPass(ID) {}

  bool doInitialization(Module &M) override {
    if (!EnableWedlockPass) {
      return false;
    }

    std::error_code StreamEC{};
    if (!WedlockLoggingOutput.empty()) {
        WedlockLoggingStream =
            new raw_fd_ostream(WedlockLoggingOutput, StreamEC,
                               sys::fs::CD_CreateAlways, sys::fs::FA_Write,
                               sys::fs::OF_None);

        if (StreamEC) {
          report_fatal_error("Failed to open " + WedlockLoggingOutput, false);
        }
    }

    WedlockStream =
        new raw_fd_ostream(WedlockOutput, StreamEC, sys::fs::OF_None);

    if (StreamEC) {
      report_fatal_error("Failed to open " + WedlockOutput, false);
    }

    return false;
  }

  bool runOnMachineFunction(MachineFunction &MF) override {
    if (!EnableWedlockPass) {
      return false;
    }

    doWedlockPairs(MF);

    return false;
  }

  bool doFinalization(Module &M) override {
    if (!EnableWedlockPass) {
      return false;
    }

    if (WedlockLoggingStream != nullptr) {
        delete WedlockLoggingStream;
    }

    delete WedlockStream;
    return false;
  }

private:
  raw_ostream &verboses() {
    if (WedlockLoggingStream != nullptr) {
      return *WedlockLoggingStream;
    } else {
      return nulls();
    }
  }

  /* NOTE(ww): Stolen from Demangle.cpp (where it's static in LLVM 10).
   */
  static bool isItaniumEncoding(const std::string &MangledName) {
    size_t Pos = MangledName.find_first_not_of('_');
    // A valid Itanium encoding requires 1-4 leading underscores, followed by 'Z'.
    return Pos > 0 && Pos <= 4 && MangledName[Pos] == 'Z';
  }

  void doWedlockPairs(MachineFunction &MF) {
    const auto *TII = MF.getSubtarget().getInstrInfo();
    const auto *Mod = MF.getMMI().getModule();

    if (!TII || !Mod) {
      verboses() << "No TargetInstrInfo or Module for this machine function?\n";
      return;
    }

    std::string BackingStr;
    raw_string_ostream RSO(BackingStr);

    auto BasicBlocksJson = JArray{};
    for (auto &MBB : MF) {
      JObject BasicBlockJson{};

      const auto BB = MBB.getBasicBlock();
      if (BB != nullptr) {
        BB->printAsOperand(RSO, false);
        const auto BBOperand(RSO.str());
        BackingStr.clear();
        BasicBlockJson.insert({"ir", JObject{
                                         {"operand", std::move(BBOperand)},
                                     }});
      } else {
        verboses() << "No IR BB for this machine BB; emitting partial!\n";
      }

      // TODO(ww): Memory efficiency. This is probably very slow.
      JArray MIPrettyInstrs{};
      JArray MIInstrs{};
      bool HasInlineAsm(false);
      for (auto &MI : MBB) {
        if (EnableMIPrettyPrinting) {
          MI.print(RSO, false, /* IsStandalone */
                   false,      /* SkipOpers */
                   false,      /* SkipDebugLoc */
                   false,      /* AddNewLine */
                   TII);

          MIPrettyInstrs.push_back(RSO.str());
          BackingStr.clear();
        }

        // TODO(ww): MI.getDesc?
        // TODO(ww): Maybe expose MIFlag bits?
        // e.g. MI.getFlags() & MachineInstr::MIFlag::FrameSetup
        // TODO(ww): MI.getFoldedSpillSize()
        // TODO(ww): Iterate over operands
        MIInstrs.push_back(JObject{
            {"opcode", MI.getOpcode()},
            {"flags", MI.getFlags()},
        });

        if (MI.isInlineAsm()) {
          HasInlineAsm = true;
        }
      }

      JArray MIPreds{};
      for (const auto *MIP : MBB.predecessors()) {
        if (MIP == nullptr) {
          verboses() << "Weird: null predecessor for MBB?\n";
          continue;
        }

        MIPreds.push_back(JObject{
            {"number", MIP->getNumber()},
            {"symbol", MIP->getSymbol()->getName()},
        });
      }

      JArray MISuccs{};
      for (const auto *MIS : MBB.successors()) {
        if (MIS == nullptr) {
          verboses() << "Weird: null successor for MBB?\n";
          continue;
        }

        MISuccs.push_back(JObject{
            {"number", MIS->getNumber()},
            {"symbol", MIS->getSymbol()->getName()},
            {"layout_successor", MBB.isLayoutSuccessor(MIS)},
        });
      }

      BasicBlockJson.insert(
          {"mi", JObject{
                     {"number", MBB.getNumber()},
                     {"symbol", MBB.getSymbol()->getName()},
                     {"can_fallthrough", MBB.canFallThrough()},
                     {"ends_in_return", MBB.isReturnBlock()},
                     {"address_taken", MBB.hasAddressTaken()},
                     {"has_inline_asm", HasInlineAsm},
                     {"preds", std::move(MIPreds)},
                     {"succs", std::move(MISuccs)},
                     {"instrs", std::move(MIInstrs)},
                     {"asm", std::move(MIPrettyInstrs)},
                 }});

      BasicBlocksJson.push_back(std::move(BasicBlockJson));
    }

    MF.getFunction().printAsOperand(RSO, false);
    const auto FuncOperand(RSO.str());

    const auto &MFI = MF.getFrameInfo();
    JValue FrameInfoJson = JObject{
      {"has_stack_objects", MFI.hasStackObjects()},
      {"has_variadic_objects", MFI.hasVarSizedObjects()},
      {"is_frame_address_taken", MFI.isFrameAddressTaken()},
      {"is_return_address_taken", MFI.isReturnAddressTaken()},
      {"num_objects", MFI.getNumObjects()},
      {"num_fixed_objects", MFI.getNumFixedObjects()},
      {"stack_size", static_cast<int64_t>(MFI.getStackSize())},
      {"adjusts_stack", MFI.adjustsStack()},
    };

    JValue WedlockJson =
        JObject{{"function",
                 JObject{
                     {"operand", FuncOperand},
                     {"name", MF.getName()},
                     {"is_mangled", isItaniumEncoding(MF.getName())},
                     {"demangled_name", demangle(MF.getName()),
                     {"frame_info", std::move(FrameInfoJson)},
                     {"bbs", std::move(BasicBlocksJson)},
                 }},
                {"module",
                 JObject{
                     {"module_name", Mod->getName()},
                     {"module_stem", sys::path::stem(Mod->getName())},
                     {"source_name", Mod->getSourceFileName()},
                     {"source_stem", sys::path::stem(Mod->getSourceFileName())},
                 }}};

    *WedlockStream << WedlockJson << '\n';
  }
};
} // namespace

char WedlockPass::ID = 0;

namespace llvm {
MachineFunctionPass *createWedlockPass() { return new WedlockPass(); }
} // namespace llvm
