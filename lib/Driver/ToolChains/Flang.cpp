//===--- Flang.cpp - Flang+LLVM ToolChain Implementations -------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Flang.h"
#include "CommonArgs.h"
#include "InputInfo.h"
#include "clang/Basic/CharInfo.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/ObjCRuntime.h"
#include "clang/Basic/Version.h"
#include "clang/Config/config.h"
#include "clang/Driver/DriverDiagnostic.h"
#include "clang/Driver/Options.h"
#include "clang/Driver/SanitizerArgs.h"
#include "clang/Driver/XRayArgs.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/Compression.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/TargetParser.h"
#include "llvm/Support/YAMLParser.h"

#ifdef LLVM_ON_UNIX
#include <unistd.h> // For getuid().
#endif

using namespace clang::driver;
using namespace clang::driver::tools;
using namespace clang;
using namespace llvm::opt;

void FlangFrontend::ConstructJob(Compilation &C, const JobAction &JA,
                         const InputInfo &Output, const InputInfoList &Inputs,
                         const ArgList &Args, const char *LinkingOutput) const {
  ArgStringList CommonCmdArgs;
  ArgStringList UpperCmdArgs;
  ArgStringList LowerCmdArgs;
  SmallString<256> Stem;
  std::string OutFile;
  bool NeedIEEE = true;

  // Check number of inputs for sanity. We need at least one input.
  assert(Inputs.size() >= 1 && "Must have at least one input.");

  /***** Process file arguments to both parts *****/
  const InputInfo &Input = Inputs[0];
  types::ID InputType = Input.getType();
  // Check file type sanity
  assert(types::isFortran(InputType) && "Can only accept Fortran");

  if (Args.hasArg(options::OPT_fsyntax_only) ||
      Args.hasArg(options::OPT_E)) {
    // For -fsyntax-only produce temp files only
    Stem = C.getDriver().GetTemporaryPath("", "");
  } else {
    OutFile = Output.getFilename();
    Stem = llvm::sys::path::filename(OutFile);
    llvm::sys::path::replace_extension(Stem, "");
  }

  // Add input file name to the compilation line
  UpperCmdArgs.push_back(Input.getBaseInput());

  // Add temporary output for ILM
  const char * ILMFile = Args.MakeArgString(Stem + ".ilm");
  LowerCmdArgs.push_back(ILMFile);
  C.addTempFile(ILMFile);

  /***** Process common args *****/

  // Override IEEE mode if needed
  if (Args.hasArg(options::OPT_Ofast) ||
      Args.hasArg(options::OPT_ffast_math) ||
      Args.hasArg(options::OPT_fno_fast_math) ||
      Args.hasArg(options::OPT_Kieee_on) ||
      Args.hasArg(options::OPT_Kieee_off)) {
    auto A = Args.getLastArg(options::OPT_Ofast,
                             options::OPT_ffast_math,
                             options::OPT_fno_fast_math,
                             options::OPT_Kieee_on,
                             options::OPT_Kieee_off);
    auto Opt = A->getOption();
    if (Opt.matches(options::OPT_Ofast) ||
        Opt.matches(options::OPT_ffast_math) ||
        Opt.matches(options::OPT_Kieee_off)) {
      NeedIEEE = false;
    }
  }

  // -Kieee is on by default
  if (!Args.hasArg(options::OPT_Kieee_off)) {
    CommonCmdArgs.push_back("-y"); // Common: -y 129 2
    CommonCmdArgs.push_back("129");
    CommonCmdArgs.push_back("2");
    // Lower: -x 6 0x100
    LowerCmdArgs.push_back("-x");
    LowerCmdArgs.push_back("6");
    LowerCmdArgs.push_back("0x100");
    // -x 42 0x400000
    LowerCmdArgs.push_back("-x");
    LowerCmdArgs.push_back("42");
    LowerCmdArgs.push_back("0x400000");
    // -y 129 4
    LowerCmdArgs.push_back("-y");
    LowerCmdArgs.push_back("129");
    LowerCmdArgs.push_back("4");
    // -x 129 0x400
    LowerCmdArgs.push_back("-x");
    LowerCmdArgs.push_back("129");
    LowerCmdArgs.push_back("0x400");
    for (auto Arg : Args.filtered(options::OPT_Kieee_on)) {
      Arg->claim();
    }
  } else {
    for (auto Arg : Args.filtered(options::OPT_Kieee_off)) {
      Arg->claim();
    }
  }

  // Add "inform level" flag
  if (Args.hasArg(options::OPT_Minform_EQ)) {
    // Parse arguments to set its value
    for (Arg *A : Args.filtered(options::OPT_Minform_EQ)) {
      A->claim();
      CommonCmdArgs.push_back("-inform");
      CommonCmdArgs.push_back(A->getValue(0));
    }
  } else {
    // Default value
    CommonCmdArgs.push_back("-inform");
    CommonCmdArgs.push_back("warn");
  }

  for (auto Arg : Args.filtered(options::OPT_Msave_on)) {
    Arg->claim();
    CommonCmdArgs.push_back("-save");
  }

  for (auto Arg : Args.filtered(options::OPT_Msave_off)) {
    Arg->claim();
    CommonCmdArgs.push_back("-nosave");
  }

  // Treat denormalized numbers as zero: On
  for (auto Arg : Args.filtered(options::OPT_Mdaz_on)) {
    Arg->claim();
    CommonCmdArgs.push_back("-x");
    CommonCmdArgs.push_back("129");
    CommonCmdArgs.push_back("4");
    CommonCmdArgs.push_back("-y");
    CommonCmdArgs.push_back("129");
    CommonCmdArgs.push_back("0x400");
  }

  // Treat denormalized numbers as zero: Off
  for (auto Arg : Args.filtered(options::OPT_Mdaz_off)) {
    Arg->claim();
    CommonCmdArgs.push_back("-y");
    CommonCmdArgs.push_back("129");
    CommonCmdArgs.push_back("4");
    CommonCmdArgs.push_back("-x");
    CommonCmdArgs.push_back("129");
    CommonCmdArgs.push_back("0x400");
  }

  // Bounds checking: On
  for (auto Arg : Args.filtered(options::OPT_Mbounds_on)) {
    Arg->claim();
    CommonCmdArgs.push_back("-x");
    CommonCmdArgs.push_back("70");
    CommonCmdArgs.push_back("2");
  }

  // Bounds checking: Off
  for (auto Arg : Args.filtered(options::OPT_Mbounds_off)) {
    Arg->claim();
    CommonCmdArgs.push_back("-y");
    CommonCmdArgs.push_back("70");
    CommonCmdArgs.push_back("2");
  }

  // Generate code allowing recursive subprograms
  for (auto Arg : Args.filtered(options::OPT_Mrecursive_on)) {
    Arg->claim();
    CommonCmdArgs.push_back("-recursive");
  }

  // Disable recursive subprograms
  for (auto Arg : Args.filtered(options::OPT_Mrecursive_off)) {
    Arg->claim();
    CommonCmdArgs.push_back("-norecursive");
  }

  // Enable generating reentrant code (disable optimizations that inhibit it)
  for (auto Arg : Args.filtered(options::OPT_Mreentrant_on)) {
    Arg->claim();
    CommonCmdArgs.push_back("-reentrant");
  }

  // Allow optimizations inhibiting reentrancy
  for (auto Arg : Args.filtered(options::OPT_Mreentrant_off)) {
    Arg->claim();
    CommonCmdArgs.push_back("-noreentrant");
  }

  // Swap byte order for unformatted IO
  for (auto Arg : Args.filtered(options::OPT_Mbyteswapio, options::OPT_byteswapio)) {
    Arg->claim();
    CommonCmdArgs.push_back("-x");
    CommonCmdArgs.push_back("125");
    CommonCmdArgs.push_back("2");
  }

  // Treat backslashes as regular characters
  for (auto Arg : Args.filtered(options::OPT_fnobackslash, options::OPT_Mbackslash)) {
    Arg->claim();
    CommonCmdArgs.push_back("-x");
    CommonCmdArgs.push_back("124");
    CommonCmdArgs.push_back("0x40");
  }

  // Treat backslashes as C-style escape characters
  for (auto Arg : Args.filtered(options::OPT_fbackslash, options::OPT_Mnobackslash)) {
    Arg->claim();
    CommonCmdArgs.push_back("-y");
    CommonCmdArgs.push_back("124");
    CommonCmdArgs.push_back("0x40");
  }

  // handle OpemMP options
  if (auto *A = Args.getLastArg(options::OPT_mp, options::OPT_nomp,
                             options::OPT_fopenmp, options::OPT_fno_openmp)) {
    for (auto Arg : Args.filtered(options::OPT_mp, options::OPT_nomp)) {
      Arg->claim();
    }
    for (auto Arg : Args.filtered(options::OPT_fopenmp,
                                  options::OPT_fno_openmp)) {
      Arg->claim();
    }

    if (A->getOption().matches(options::OPT_mp) ||
        A->getOption().matches(options::OPT_fopenmp)) {

      CommonCmdArgs.push_back("-mp");

       // Allocate threadprivate data local to the thread
      CommonCmdArgs.push_back("-x");
      CommonCmdArgs.push_back("69");
      CommonCmdArgs.push_back("0x200");

      // Use the 'fair' schedule as the default static schedule
      // for parallel do loops
      CommonCmdArgs.push_back("-x");
      CommonCmdArgs.push_back("69");
      CommonCmdArgs.push_back("0x400");
    }
  }

  // Align large objects on cache lines
  for (auto Arg : Args.filtered(options::OPT_Mcache_align_on)) {
    Arg->claim();
    CommonCmdArgs.push_back("-x");
    CommonCmdArgs.push_back("119");
    CommonCmdArgs.push_back("0x10000000");
    LowerCmdArgs.push_back("-x");
    LowerCmdArgs.push_back("129");
    LowerCmdArgs.push_back("0x40000000");
  }

  // Disable special alignment of large objects
  for (auto Arg : Args.filtered(options::OPT_Mcache_align_off)) {
    Arg->claim();
    CommonCmdArgs.push_back("-y");
    CommonCmdArgs.push_back("119");
    CommonCmdArgs.push_back("0x10000000");
    LowerCmdArgs.push_back("-y");
    LowerCmdArgs.push_back("129");
    LowerCmdArgs.push_back("0x40000000");
  }

  // -Mstack_arrays
  for (auto Arg : Args.filtered(options::OPT_Mstackarrays)) {
    Arg->claim();
    CommonCmdArgs.push_back("-x");
    CommonCmdArgs.push_back("54");
    CommonCmdArgs.push_back("8");
  }

  // -g should produce DWARFv2
  for (auto Arg : Args.filtered(options::OPT_g_Flag)) {
    Arg->claim();
    CommonCmdArgs.push_back("-x");
    CommonCmdArgs.push_back("120");
    CommonCmdArgs.push_back("0x200");
  }

  // -gdwarf-2
  for (auto Arg : Args.filtered(options::OPT_gdwarf_2)) {
    Arg->claim();
    CommonCmdArgs.push_back("-x");
    CommonCmdArgs.push_back("120");
    CommonCmdArgs.push_back("0x200");
  }

  // -gdwarf-3
  for (auto Arg : Args.filtered(options::OPT_gdwarf_3)) {
    Arg->claim();
    CommonCmdArgs.push_back("-x");
    CommonCmdArgs.push_back("120");
    CommonCmdArgs.push_back("0x4000");
  }

  // -Mipa has no effect
  if (Arg *A = Args.getLastArg(options::OPT_Mipa)) {
    getToolChain().getDriver().Diag(diag::warn_drv_clang_unsupported)
      << A->getAsString(Args);
  }

  // -Minline has no effect
  if (Arg *A = Args.getLastArg(options::OPT_Minline_on)) {
    getToolChain().getDriver().Diag(diag::warn_drv_clang_unsupported)
      << A->getAsString(Args);
  }

  // Handle -fdefault-real-8 (and its alias, -r8) and -fno-default-real-8
  if (Arg *A = Args.getLastArg(options::OPT_r8,
                               options::OPT_default_real_8_f,
                               options::OPT_default_real_8_fno)) {
    const char * fl;
    // For -f version add -x flag, for -fno add -y
    if (A->getOption().matches(options::OPT_default_real_8_fno)) {
      fl = "-y";
    } else {
      fl = "-x";
    }

    for (Arg *A : Args.filtered(options::OPT_r8,
                                options::OPT_default_real_8_f,
                                options::OPT_default_real_8_fno)) {
      A->claim();
    }

    UpperCmdArgs.push_back(fl);
    UpperCmdArgs.push_back("124");
    UpperCmdArgs.push_back("0x8");
    UpperCmdArgs.push_back(fl);
    UpperCmdArgs.push_back("124");
    UpperCmdArgs.push_back("0x80000");
  }

  // Process and claim -i8/-fdefault-integer-8/-fno-default-integer-8 argument
  if (Arg *A = Args.getLastArg(options::OPT_i8,
                               options::OPT_default_integer_8_f,
                               options::OPT_default_integer_8_fno)) {
    const char * fl;

    if (A->getOption().matches(options::OPT_default_integer_8_fno)) {
      fl = "-y";
    } else {
      fl = "-x";
    }

    for (Arg *A : Args.filtered(options::OPT_i8,
                                options::OPT_default_integer_8_f,
                                options::OPT_default_integer_8_fno)) {
      A->claim();
    }

    UpperCmdArgs.push_back(fl);
    UpperCmdArgs.push_back("124");
    UpperCmdArgs.push_back("0x10");
  }

  // Set a -x flag for first part of Fortran frontend
  for (Arg *A : Args.filtered(options::OPT_Hx_EQ)) {
    A->claim();
    StringRef Value = A->getValue();
    auto XFlag = Value.split(",");
    UpperCmdArgs.push_back("-x");
    UpperCmdArgs.push_back(Args.MakeArgString(XFlag.first));
    UpperCmdArgs.push_back(Args.MakeArgString(XFlag.second));
  }

  // Set a -y flag for first part of Fortran frontend
  for (Arg *A : Args.filtered(options::OPT_Hy_EQ)) {
    A->claim();
    StringRef Value = A->getValue();
    auto XFlag = Value.split(",");
    UpperCmdArgs.push_back("-y");
    UpperCmdArgs.push_back(Args.MakeArgString(XFlag.first));
    UpperCmdArgs.push_back(Args.MakeArgString(XFlag.second));
  }

  // Set a -q (debug) flag for first part of Fortran frontend
  for (Arg *A : Args.filtered(options::OPT_Hq_EQ)) {
    A->claim();
    StringRef Value = A->getValue();
    auto XFlag = Value.split(",");
    UpperCmdArgs.push_back("-q");
    UpperCmdArgs.push_back(Args.MakeArgString(XFlag.first));
    UpperCmdArgs.push_back(Args.MakeArgString(XFlag.second));
  }

  // Set a -qq (debug) flag for first part of Fortran frontend
  for (Arg *A : Args.filtered(options::OPT_Hqq_EQ)) {
    A->claim();
    StringRef Value = A->getValue();
    auto XFlag = Value.split(",");
    UpperCmdArgs.push_back("-qq");
    UpperCmdArgs.push_back(Args.MakeArgString(XFlag.first));
    UpperCmdArgs.push_back(Args.MakeArgString(XFlag.second));
  }

  // Pass an arbitrary flag for first part of Fortran frontend
  for (Arg *A : Args.filtered(options::OPT_Wh_EQ)) {
    A->claim();
    StringRef Value = A->getValue();
    SmallVector<StringRef, 8> PassArgs;
    Value.split(PassArgs, StringRef(","));
    for (StringRef PassArg : PassArgs) {
      UpperCmdArgs.push_back(Args.MakeArgString(PassArg));
    }
  }

  // Flush to zero mode
  // Disabled by default, but can be enabled by a switch
  if (Args.hasArg(options::OPT_Mflushz_on)) {
    // For -Mflushz set -x 129 2 for second part of Fortran frontend
    for (Arg *A: Args.filtered(options::OPT_Mflushz_on)) {
      A->claim();
      LowerCmdArgs.push_back("-x");
      LowerCmdArgs.push_back("129");
      LowerCmdArgs.push_back("2");
    }
  } else {
    LowerCmdArgs.push_back("-y");
    LowerCmdArgs.push_back("129");
    LowerCmdArgs.push_back("2");
    for (Arg *A: Args.filtered(options::OPT_Mflushz_off)) {
      A->claim();
    }
  }

  // Enable FMA
  for (Arg *A: Args.filtered(options::OPT_Mfma_on, options::OPT_fma)) {
    A->claim();
    LowerCmdArgs.push_back("-x");
    LowerCmdArgs.push_back("172");
    LowerCmdArgs.push_back("0x40000000");
    LowerCmdArgs.push_back("-x");
    LowerCmdArgs.push_back("179");
    LowerCmdArgs.push_back("1");
  }

  // Disable FMA
  for (Arg *A: Args.filtered(options::OPT_Mfma_off, options::OPT_nofma)) {
    A->claim();
    LowerCmdArgs.push_back("-x");
    LowerCmdArgs.push_back("171");
    LowerCmdArgs.push_back("0x40000000");
    LowerCmdArgs.push_back("-x");
    LowerCmdArgs.push_back("178");
    LowerCmdArgs.push_back("1");
  }

  // For -fPIC set -x 62 8 for second part of Fortran frontend
  for (Arg *A: Args.filtered(options::OPT_fPIC)) {
    A->claim();
    LowerCmdArgs.push_back("-x");
    LowerCmdArgs.push_back("62");
    LowerCmdArgs.push_back("8");
  }

  StringRef OptOStr("0");
  if (Arg *A = Args.getLastArg(options::OPT_O_Group)) {
    if (A->getOption().matches(options::OPT_O4)) {
      OptOStr = "4"; // FIXME what should this be?
    } else if (A->getOption().matches(options::OPT_Ofast)) {
      OptOStr = "2"; // FIXME what should this be?
    } else if (A->getOption().matches(options::OPT_O0)) {
      // intentionally do nothing
    } else {
      assert(A->getOption().matches(options::OPT_O) && "Must have a -O flag");
      StringRef S(A->getValue());
      if ((S == "s") || (S == "z")) {
	// -Os = size; -Oz = more size
	OptOStr = "2"; // FIXME -Os|-Oz => -opt ?
      } else if ((S == "1") || (S == "2") || (S == "3")) {
	OptOStr = S;
      } else {
	OptOStr = "4";
      }
    }
  }
  unsigned OptLevel = std::stoi(OptOStr.str());

  if (Args.hasArg(options::OPT_g_Group)) {
    // pass -g to lower
    LowerCmdArgs.push_back("-debug");
  }

  if (Args.hasArg(options::OPT_finstrument_functions)) {
      LowerCmdArgs.push_back("-x");
      LowerCmdArgs.push_back("129");
      LowerCmdArgs.push_back("0x800");
  }

  if (Arg *A = Args.getLastArg(options::OPT_ffast_math, options::OPT_fno_fast_math)) {
    if (A->getOption().matches(options::OPT_ffast_math)) {
      LowerCmdArgs.push_back("-x");
    } else {
      LowerCmdArgs.push_back("-y");
    }
    LowerCmdArgs.push_back("216");
    LowerCmdArgs.push_back("1");
  }

  // IEEE compatibility mode
  LowerCmdArgs.push_back("-ieee");
  if (NeedIEEE) {
    LowerCmdArgs.push_back("1");
  } else {
    LowerCmdArgs.push_back("0");
  }

  /***** Upper part of the Fortran frontend *****/

  // TODO do we need to invoke this under GDB sometimes?
  const char *UpperExec = Args.MakeArgString(getToolChain().GetProgramPath("flang1"));

  UpperCmdArgs.push_back("-opt"); UpperCmdArgs.push_back(Args.MakeArgString(OptOStr));
  UpperCmdArgs.push_back("-terse"); UpperCmdArgs.push_back("1");
  UpperCmdArgs.push_back("-inform"); UpperCmdArgs.push_back("warn");
  UpperCmdArgs.push_back("-nohpf");
  UpperCmdArgs.push_back("-nostatic");
  UpperCmdArgs.append(CommonCmdArgs.begin(), CommonCmdArgs.end()); // Append common arguments
  UpperCmdArgs.push_back("-x"); UpperCmdArgs.push_back("19"); UpperCmdArgs.push_back("0x400000");
  UpperCmdArgs.push_back("-quad");
  UpperCmdArgs.push_back("-x"); UpperCmdArgs.push_back("59"); UpperCmdArgs.push_back("4");
  UpperCmdArgs.push_back("-x"); UpperCmdArgs.push_back("15"); UpperCmdArgs.push_back("2");
  UpperCmdArgs.push_back("-x"); UpperCmdArgs.push_back("49"); UpperCmdArgs.push_back("0x400004");
  UpperCmdArgs.push_back("-x"); UpperCmdArgs.push_back("51"); UpperCmdArgs.push_back("0x20");
  UpperCmdArgs.push_back("-x"); UpperCmdArgs.push_back("57"); UpperCmdArgs.push_back("0x4c");
  UpperCmdArgs.push_back("-x"); UpperCmdArgs.push_back("58"); UpperCmdArgs.push_back("0x10000");
  UpperCmdArgs.push_back("-x"); UpperCmdArgs.push_back("124"); UpperCmdArgs.push_back("0x1000");
  UpperCmdArgs.push_back("-tp"); UpperCmdArgs.push_back("px");
  UpperCmdArgs.push_back("-x"); UpperCmdArgs.push_back("57"); UpperCmdArgs.push_back("0xfb0000");
  UpperCmdArgs.push_back("-x"); UpperCmdArgs.push_back("58"); UpperCmdArgs.push_back("0x78031040");
  UpperCmdArgs.push_back("-x"); UpperCmdArgs.push_back("47"); UpperCmdArgs.push_back("0x08");
  UpperCmdArgs.push_back("-x"); UpperCmdArgs.push_back("48"); UpperCmdArgs.push_back("4608");
  UpperCmdArgs.push_back("-x"); UpperCmdArgs.push_back("49"); UpperCmdArgs.push_back("0x100");
  if (OptLevel >= 2) {
    UpperCmdArgs.push_back("-x");
    UpperCmdArgs.push_back("70");
    UpperCmdArgs.push_back("0x6c00");
    UpperCmdArgs.push_back("-x");
    UpperCmdArgs.push_back("119");
    UpperCmdArgs.push_back("0x10000000");
    UpperCmdArgs.push_back("-x");
    UpperCmdArgs.push_back("129");
    UpperCmdArgs.push_back("2");
    UpperCmdArgs.push_back("-x");
    UpperCmdArgs.push_back("47");
    UpperCmdArgs.push_back("0x400000");
    UpperCmdArgs.push_back("-x");
    UpperCmdArgs.push_back("52");
    UpperCmdArgs.push_back("2");
  }

  // Add system include arguments.
  getToolChain().AddFlangSystemIncludeArgs(Args, UpperCmdArgs);
	
  bool IsWindowsMSVC = getToolChain().getTriple().isWindowsMSVCEnvironment();

  if (!IsWindowsMSVC) {
  UpperCmdArgs.push_back("-def"); UpperCmdArgs.push_back("unix");
  UpperCmdArgs.push_back("-def"); UpperCmdArgs.push_back("__unix");
  UpperCmdArgs.push_back("-def"); UpperCmdArgs.push_back("__unix__");
  UpperCmdArgs.push_back("-def"); UpperCmdArgs.push_back("linux");
  UpperCmdArgs.push_back("-def"); UpperCmdArgs.push_back("__linux");
  UpperCmdArgs.push_back("-def"); UpperCmdArgs.push_back("__linux__");
  UpperCmdArgs.push_back("-def"); UpperCmdArgs.push_back("__LP64__");
  UpperCmdArgs.push_back("-def"); UpperCmdArgs.push_back("__LONG_MAX__=9223372036854775807L");
  UpperCmdArgs.push_back("-def"); UpperCmdArgs.push_back("__SIZE_TYPE__=unsigned long int");
  UpperCmdArgs.push_back("-def"); UpperCmdArgs.push_back("__PTRDIFF_TYPE__=long int");
  } else {
  UpperCmdArgs.push_back("-def"); UpperCmdArgs.push_back("__LONG_MAX__=2147483647L");
  UpperCmdArgs.push_back("-def"); UpperCmdArgs.push_back("__SIZE_TYPE__=unsigned long long int");
  UpperCmdArgs.push_back("-def"); UpperCmdArgs.push_back("__PTRDIFF_TYPE__=long long int");
  UpperCmdArgs.push_back("-def"); UpperCmdArgs.push_back("_WIN32");
  UpperCmdArgs.push_back("-def"); UpperCmdArgs.push_back("WIN32");
  UpperCmdArgs.push_back("-def"); UpperCmdArgs.push_back("_WIN64");
  UpperCmdArgs.push_back("-def"); UpperCmdArgs.push_back("WIN64");
  VersionTuple MSVT =
       getToolChain().computeMSVCVersion(&getToolChain().getDriver(), Args);
  auto msc_ver = MSVT.getMajor() * 100 + MSVT.getMinor().getValueOr(0);
  UpperCmdArgs.push_back("-def");
  UpperCmdArgs.push_back(Args.MakeArgString(std::string("_MSC_VER=")+std::to_string(msc_ver)));
  }
  UpperCmdArgs.push_back("-def"); UpperCmdArgs.push_back("__NO_MATH_INLINES");
  UpperCmdArgs.push_back("-def"); UpperCmdArgs.push_back("__x86_64");
  UpperCmdArgs.push_back("-def"); UpperCmdArgs.push_back("__x86_64__");
  UpperCmdArgs.push_back("-def"); UpperCmdArgs.push_back("__THROW=");
  UpperCmdArgs.push_back("-def"); UpperCmdArgs.push_back("__extension__=");
  UpperCmdArgs.push_back("-def"); UpperCmdArgs.push_back("__amd_64__amd64__");
  UpperCmdArgs.push_back("-def"); UpperCmdArgs.push_back("__k8");
  UpperCmdArgs.push_back("-def"); UpperCmdArgs.push_back("__k8__");
  UpperCmdArgs.push_back("-def"); UpperCmdArgs.push_back("__PGLLVM__");

  /*
    When the -E option is given, run flang1 in preprocessor only mode
  */
  if (Args.hasArg(options::OPT_E)) {
    UpperCmdArgs.push_back("-es");
  }

  // Enable preprocessor
  if (Args.hasArg(options::OPT_E) ||
      Args.hasArg(options::OPT_Mpreprocess) ||
      Args.hasArg(options::OPT_cpp) ||
      types::getPreprocessedType(InputType) != types::TY_INVALID) {
    UpperCmdArgs.push_back("-preprocess");
    for (auto Arg : Args.filtered(options::OPT_E, options::OPT_Mpreprocess,
                                  options::OPT_cpp)) {
      Arg->claim();
    }
  }

  // Enable standards checking
  if (Args.hasArg(options::OPT_Mstandard)) {
    UpperCmdArgs.push_back("-standard");
    for (auto Arg : Args.filtered(options::OPT_Mstandard)) {
      Arg->claim();
    }
  }

  // Free or fixed form file
  if (Args.hasArg(options::OPT_fortran_format_Group)) {
    // Override file name suffix, scan arguments for that
    for (Arg *A : Args.filtered(options::OPT_fortran_format_Group)) {
      A->claim();
      switch (A->getOption().getID()) {
        default:
          llvm_unreachable("missed a case");
         case options::OPT_fixed_form_on:
         case options::OPT_free_form_off:
         case options::OPT_Mfixed:
         case options::OPT_Mfree_off:
         case options::OPT_Mfreeform_off:
           UpperCmdArgs.push_back("-nofreeform");
           break;
         case options::OPT_free_form_on:
         case options::OPT_fixed_form_off:
         case options::OPT_Mfree_on:
         case options::OPT_Mfreeform_on:
           UpperCmdArgs.push_back("-freeform");
           break;
      }
    }
  } else {
    // Deduce format from file name suffix
    if (types::isFreeFormFortran(InputType)) {
      UpperCmdArgs.push_back("-freeform");
    } else {
      UpperCmdArgs.push_back("-nofreeform");
    }
  }

  // Extend lines to 132 characters
  for (auto Arg : Args.filtered(options::OPT_Mextend)) {
    Arg->claim();
    UpperCmdArgs.push_back("-extend");
  }

  for (auto Arg : Args.filtered(options::OPT_ffixed_line_length_VALUE)) {
    StringRef Value = Arg->getValue();
    if (Value == "72") {
      Arg->claim();
    } else if (Value == "132") {
      Arg->claim();
      UpperCmdArgs.push_back("-extend");
    } else {
      getToolChain().getDriver().Diag(diag::err_drv_unsupported_fixed_line_length)
        << Arg->getAsString(Args);
    }
  }

  // Add user-defined include directories
  for (auto Arg : Args.filtered(options::OPT_I)) {
    Arg->claim();
    UpperCmdArgs.push_back("-idir");
    UpperCmdArgs.push_back(Arg->getValue(0));
  }

  // Add user-defined module directories
  for (auto Arg : Args.filtered(options::OPT_ModuleDir, options::OPT_J)) {
    Arg->claim();
    UpperCmdArgs.push_back("-moddir");
    UpperCmdArgs.push_back(Arg->getValue(0));
  }

  // Add env variables
  addDirectoryList(Args, UpperCmdArgs, "-idir", "C_INCLUDE_PATH");
  addDirectoryList(Args, UpperCmdArgs, "-idir", "CPATH");	

  // "Define" preprocessor flags
  for (auto Arg : Args.filtered(options::OPT_D)) {
    Arg->claim();
    UpperCmdArgs.push_back("-def");
    UpperCmdArgs.push_back(Arg->getValue(0));
  }

  // "Define" preprocessor flags
  for (auto Arg : Args.filtered(options::OPT_U)) {
    Arg->claim();
    UpperCmdArgs.push_back("-undef");
    UpperCmdArgs.push_back(Arg->getValue(0));
  }

  UpperCmdArgs.push_back("-vect"); UpperCmdArgs.push_back("48");

  // Semantics for assignments to allocatables
  if (Arg *A = Args.getLastArg(options::OPT_Mallocatable_EQ)) {
    // Argument is passed explicitly
    StringRef Value = A->getValue();
    if (Value == "03") { // Enable Fortran 2003 semantics
      UpperCmdArgs.push_back("-x"); // Set XBIT
    } else if (Value == "95") { // Enable Fortran 2003 semantics
      UpperCmdArgs.push_back("-y"); // Unset XBIT
    } else {
      getToolChain().getDriver().Diag(diag::err_drv_invalid_allocatable_mode)
        << A->getAsString(Args);
    }
  } else { // No argument passed
    UpperCmdArgs.push_back("-y"); // Default is 1995
  }
  UpperCmdArgs.push_back("54"); UpperCmdArgs.push_back("1"); // XBIT value

  UpperCmdArgs.push_back("-x"); UpperCmdArgs.push_back("70"); UpperCmdArgs.push_back("0x40000000");
  UpperCmdArgs.push_back("-y"); UpperCmdArgs.push_back("163"); UpperCmdArgs.push_back("0xc0000000");
  UpperCmdArgs.push_back("-x"); UpperCmdArgs.push_back("189"); UpperCmdArgs.push_back("0x10");

  // Enable NULL pointer checking
  if (Args.hasArg(options::OPT_Mchkptr)) {
    UpperCmdArgs.push_back("-x");
    UpperCmdArgs.push_back("70");
    UpperCmdArgs.push_back("4");
    for (auto Arg : Args.filtered(options::OPT_Mchkptr)) {
      Arg->claim();
    }
  }

  const char * STBFile = Args.MakeArgString(Stem + ".stb");
  C.addTempFile(STBFile);
  UpperCmdArgs.push_back("-stbfile");
  UpperCmdArgs.push_back(STBFile);

  const char * ModuleExportFile = Args.MakeArgString(Stem + ".cmod");
  C.addTempFile(ModuleExportFile);
  UpperCmdArgs.push_back("-modexport");
  UpperCmdArgs.push_back(ModuleExportFile);

  const char * ModuleIndexFile = Args.MakeArgString(Stem + ".cmdx");
  C.addTempFile(ModuleIndexFile);
  UpperCmdArgs.push_back("-modindex");
  UpperCmdArgs.push_back(ModuleIndexFile);

  if (Args.hasArg(options::OPT_E)) {
    if (Arg *A = Args.getLastArg(options::OPT_o)) {
      UpperCmdArgs.push_back("-output");
      UpperCmdArgs.push_back(Args.MakeArgString(A->getValue()));
    }
  } else {
    UpperCmdArgs.push_back("-output");
    UpperCmdArgs.push_back(ILMFile);
  }
  C.addCommand(llvm::make_unique<Command>(JA, *this, UpperExec, UpperCmdArgs, Inputs));

  // For -fsyntax-only or -E that is it
  if (Args.hasArg(options::OPT_fsyntax_only) ||
      Args.hasArg(options::OPT_E)) return;

  /***** Lower part of Fortran frontend *****/

  const char *LowerExec = Args.MakeArgString(getToolChain().GetProgramPath("flang2"));

  // TODO FLANG arg handling
  LowerCmdArgs.push_back("-fn"); LowerCmdArgs.push_back(Input.getBaseInput());
  LowerCmdArgs.push_back("-opt"); LowerCmdArgs.push_back(Args.MakeArgString(OptOStr));
  LowerCmdArgs.push_back("-terse"); LowerCmdArgs.push_back("1");
  LowerCmdArgs.push_back("-inform"); LowerCmdArgs.push_back("warn");
  LowerCmdArgs.append(CommonCmdArgs.begin(), CommonCmdArgs.end()); // Append common arguments
  LowerCmdArgs.push_back("-x"); LowerCmdArgs.push_back("51"); LowerCmdArgs.push_back("0x20");
  LowerCmdArgs.push_back("-x"); LowerCmdArgs.push_back("119"); LowerCmdArgs.push_back("0xa10000");
  LowerCmdArgs.push_back("-x"); LowerCmdArgs.push_back("122"); LowerCmdArgs.push_back("0x40");
  LowerCmdArgs.push_back("-x"); LowerCmdArgs.push_back("123"); LowerCmdArgs.push_back("0x1000");
  LowerCmdArgs.push_back("-x"); LowerCmdArgs.push_back("127"); LowerCmdArgs.push_back("4");
  LowerCmdArgs.push_back("-x"); LowerCmdArgs.push_back("127"); LowerCmdArgs.push_back("17");
  LowerCmdArgs.push_back("-x"); LowerCmdArgs.push_back("19"); LowerCmdArgs.push_back("0x400000");
  LowerCmdArgs.push_back("-x"); LowerCmdArgs.push_back("28"); LowerCmdArgs.push_back("0x40000");
  LowerCmdArgs.push_back("-x"); LowerCmdArgs.push_back("120"); LowerCmdArgs.push_back("0x10000000");
  LowerCmdArgs.push_back("-x"); LowerCmdArgs.push_back("70"); LowerCmdArgs.push_back("0x8000");
  LowerCmdArgs.push_back("-x"); LowerCmdArgs.push_back("122"); LowerCmdArgs.push_back("1");
  LowerCmdArgs.push_back("-x"); LowerCmdArgs.push_back("125"); LowerCmdArgs.push_back("0x20000");
  LowerCmdArgs.push_back("-quad");
  LowerCmdArgs.push_back("-x"); LowerCmdArgs.push_back("59"); LowerCmdArgs.push_back("4");
  LowerCmdArgs.push_back("-tp"); LowerCmdArgs.push_back("px");
  LowerCmdArgs.push_back("-x"); LowerCmdArgs.push_back("120"); LowerCmdArgs.push_back("0x1000"); // debug lite
  LowerCmdArgs.push_back("-x"); LowerCmdArgs.push_back("124"); LowerCmdArgs.push_back("0x1400");
  LowerCmdArgs.push_back("-y"); LowerCmdArgs.push_back("15"); LowerCmdArgs.push_back("2");
  LowerCmdArgs.push_back("-x"); LowerCmdArgs.push_back("57"); LowerCmdArgs.push_back("0x3b0000");
  LowerCmdArgs.push_back("-x"); LowerCmdArgs.push_back("58"); LowerCmdArgs.push_back("0x48000000");
  LowerCmdArgs.push_back("-x"); LowerCmdArgs.push_back("49"); LowerCmdArgs.push_back("0x100");
  LowerCmdArgs.push_back("-astype"); LowerCmdArgs.push_back("0");
  LowerCmdArgs.push_back("-x"); LowerCmdArgs.push_back("183"); LowerCmdArgs.push_back("4");
  LowerCmdArgs.push_back("-x"); LowerCmdArgs.push_back("121"); LowerCmdArgs.push_back("0x800");
  LowerCmdArgs.push_back("-x"); LowerCmdArgs.push_back("54"); LowerCmdArgs.push_back("0x10");
  LowerCmdArgs.push_back("-x"); LowerCmdArgs.push_back("70"); LowerCmdArgs.push_back("0x40000000");
  LowerCmdArgs.push_back("-x"); LowerCmdArgs.push_back("249"); LowerCmdArgs.push_back("50"); // LLVM version
  LowerCmdArgs.push_back("-x"); LowerCmdArgs.push_back("124"); LowerCmdArgs.push_back("1");
  LowerCmdArgs.push_back("-y"); LowerCmdArgs.push_back("163"); LowerCmdArgs.push_back("0xc0000000");
  LowerCmdArgs.push_back("-x"); LowerCmdArgs.push_back("189"); LowerCmdArgs.push_back("0x10");
  LowerCmdArgs.push_back("-y"); LowerCmdArgs.push_back("189"); LowerCmdArgs.push_back("0x4000000");

  // Remove "noinline" attriblute
  LowerCmdArgs.push_back("-x"); LowerCmdArgs.push_back("183"); LowerCmdArgs.push_back("0x10");

  // Set a -x flag for second part of Fortran frontend
  for (Arg *A : Args.filtered(options::OPT_Mx_EQ)) {
    A->claim();
    StringRef Value = A->getValue();
    auto XFlag = Value.split(",");
    LowerCmdArgs.push_back("-x");
    LowerCmdArgs.push_back(Args.MakeArgString(XFlag.first));
    LowerCmdArgs.push_back(Args.MakeArgString(XFlag.second));
  }

  // Set a -y flag for second part of Fortran frontend
  for (Arg *A : Args.filtered(options::OPT_My_EQ)) {
    A->claim();
    StringRef Value = A->getValue();
    auto XFlag = Value.split(",");
    LowerCmdArgs.push_back("-y");
    LowerCmdArgs.push_back(Args.MakeArgString(XFlag.first));
    LowerCmdArgs.push_back(Args.MakeArgString(XFlag.second));
  }

  // Set a -q (debug) flag for second part of Fortran frontend
  for (Arg *A : Args.filtered(options::OPT_Mq_EQ)) {
    A->claim();
    StringRef Value = A->getValue();
    auto XFlag = Value.split(",");
    LowerCmdArgs.push_back("-q");
    LowerCmdArgs.push_back(Args.MakeArgString(XFlag.first));
    LowerCmdArgs.push_back(Args.MakeArgString(XFlag.second));
  }

  // Set a -qq (debug) flag for second part of Fortran frontend
  for (Arg *A : Args.filtered(options::OPT_Mqq_EQ)) {
    A->claim();
    StringRef Value = A->getValue();
    auto XFlag = Value.split(",");
    LowerCmdArgs.push_back("-qq");
    LowerCmdArgs.push_back(Args.MakeArgString(XFlag.first));
    LowerCmdArgs.push_back(Args.MakeArgString(XFlag.second));
  }

  // Pass an arbitrary flag for second part of Fortran frontend
  for (Arg *A : Args.filtered(options::OPT_Wm_EQ)) {
    A->claim();
    StringRef Value = A->getValue();
    SmallVector<StringRef, 8> PassArgs;
    Value.split(PassArgs, StringRef(","));
    for (StringRef PassArg : PassArgs) {
      LowerCmdArgs.push_back(Args.MakeArgString(PassArg));
    }
  }

  LowerCmdArgs.push_back("-stbfile");
  LowerCmdArgs.push_back(STBFile);

  LowerCmdArgs.push_back("-asm"); LowerCmdArgs.push_back(Args.MakeArgString(OutFile));
	
  const llvm::Triple &Triple = getToolChain().getEffectiveTriple();
  const std::string &TripleStr = Triple.getTriple();
  LowerCmdArgs.push_back("-target");
  LowerCmdArgs.push_back(Args.MakeArgString(TripleStr));
	
  if (IsWindowsMSVC && !Args.hasArg(options::OPT_noFlangLibs)) {
    getToolChain().AddFortranStdlibLibArgs(Args, LowerCmdArgs, true);
    if (needFortranMain(getToolChain().getDriver(), Args)) {
      LowerCmdArgs.push_back("-linker");
      LowerCmdArgs.push_back("/subsystem:console");
      LowerCmdArgs.push_back("-linker");
      LowerCmdArgs.push_back("/defaultlib:flangmain");
    }
  }

  for (auto Arg : Args.filtered(options::OPT_noFlangLibs)) {
    Arg->claim();
  }

  C.addCommand(llvm::make_unique<Command>(JA, *this, LowerExec, LowerCmdArgs, Inputs));
}

