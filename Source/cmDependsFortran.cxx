/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file Copyright.txt or https://cmake.org/licensing for details.  */
#include "cmDependsFortran.h"

#include "cmsys/FStream.hxx"
#include <assert.h>
#include <iostream>
#include <map>
#include <stdlib.h>
#include <string.h>
#include <utility>

#include "cmAlgorithms.h"
#include "cmFortranParser.h" /* Interface to parser object.  */
#include "cmGeneratedFileStream.h"
#include "cmLocalGenerator.h"
#include "cmMakefile.h"
#include "cmOutputConverter.h"
#include "cmStateDirectory.h"
#include "cmStateSnapshot.h"
#include "cmSystemTools.h"

// TODO: Test compiler for the case of the mod file.  Some always
// use lower case and some always use upper case.  I do not know if any
// use the case from the source code.

static void cmFortranModuleAppendUpperLower(std::string const& mod,
                                            std::string& mod_upper,
                                            std::string& mod_lower)
{
  std::string::size_type ext_len = 0;
  if (cmHasLiteralSuffix(mod, ".mod")) {
    ext_len = 4;
  } else if (cmHasLiteralSuffix(mod, ".smod")) {
    ext_len = 5;
  }
  std::string const& name = mod.substr(0, mod.size() - ext_len);
  std::string const& ext = mod.substr(mod.size() - ext_len);
  mod_upper += cmSystemTools::UpperCase(name) + ext;
  mod_lower += mod;
}

class cmDependsFortranInternals
{
public:
  // The set of modules provided by this target.
  std::set<std::string> TargetProvides;

  // Map modules required by this target to locations.
  typedef std::map<std::string, std::string> TargetRequiresMap;
  TargetRequiresMap TargetRequires;

  // Information about each object file.
  typedef std::map<std::string, cmFortranSourceInfo> ObjectInfoMap;
  ObjectInfoMap ObjectInfo;

  cmFortranSourceInfo& CreateObjectInfo(const char* obj, const char* src)
  {
    std::map<std::string, cmFortranSourceInfo>::iterator i =
      this->ObjectInfo.find(obj);
    if (i == this->ObjectInfo.end()) {
      std::map<std::string, cmFortranSourceInfo>::value_type entry(
        obj, cmFortranSourceInfo());
      i = this->ObjectInfo.insert(entry).first;
      i->second.Source = src;
    }
    return i->second;
  }
};

cmDependsFortran::cmDependsFortran()
  : Internal(nullptr)
{
}

cmDependsFortran::cmDependsFortran(cmLocalGenerator* lg)
  : cmDepends(lg)
  , Internal(new cmDependsFortranInternals)
{
  // Configure the include file search path.
  this->SetIncludePathFromLanguage("Fortran");

  // Get the list of definitions.
  std::vector<std::string> definitions;
  cmMakefile* mf = this->LocalGenerator->GetMakefile();
  if (const char* c_defines =
        mf->GetDefinition("CMAKE_TARGET_DEFINITIONS_Fortran")) {
    cmSystemTools::ExpandListArgument(c_defines, definitions);
  }

  // translate i.e. FOO=BAR to FOO and add it to the list of defined
  // preprocessor symbols
  for (std::string def : definitions) {
    std::string::size_type assignment = def.find('=');
    if (assignment != std::string::npos) {
      def = def.substr(0, assignment);
    }
    this->PPDefinitions.insert(def);
  }
}

cmDependsFortran::~cmDependsFortran()
{
  delete this->Internal;
}

bool cmDependsFortran::WriteDependencies(const std::set<std::string>& sources,
                                         const std::string& obj,
                                         std::ostream& /*makeDepends*/,
                                         std::ostream& /*internalDepends*/)
{
  // Make sure this is a scanning instance.
  if (sources.empty() || sources.begin()->empty()) {
    cmSystemTools::Error("Cannot scan dependencies without a source file.");
    return false;
  }
  if (obj.empty()) {
    cmSystemTools::Error("Cannot scan dependencies without an object file.");
    return false;
  }

  bool okay = true;
  for (std::string const& src : sources) {
    // Get the information object for this source.
    cmFortranSourceInfo& info =
      this->Internal->CreateObjectInfo(obj.c_str(), src.c_str());

    // Create the parser object. The constructor takes info by reference,
    // so we may look into the resulting objects later.
    cmFortranParser parser(this->IncludePath, this->PPDefinitions, info);

    // Push on the starting file.
    cmFortranParser_FilePush(&parser, src.c_str());

    // Parse the translation unit.
    if (cmFortran_yyparse(parser.Scanner) != 0) {
      // Failed to parse the file.  Report failure to write dependencies.
      okay = false;
      /* clang-format off */
      std::cerr <<
        "warning: failed to parse dependencies from Fortran source "
        "'" << src << "': " << parser.Error << std::endl
        ;
      /* clang-format on */
    }
  }
  return okay;
}

bool cmDependsFortran::Finalize(std::ostream& makeDepends,
                                std::ostream& internalDepends)
{
  // Prepare the module search process.
  this->LocateModules();

  // Get the directory in which stamp files will be stored.
  const char* stamp_dir = this->TargetDirectory.c_str();

  // Get the directory in which module files will be created.
  cmMakefile* mf = this->LocalGenerator->GetMakefile();
  std::string mod_dir =
    mf->GetSafeDefinition("CMAKE_Fortran_TARGET_MODULE_DIR");
  if (mod_dir.empty()) {
    mod_dir = this->LocalGenerator->GetCurrentBinaryDirectory();
  }

  // Actually write dependencies to the streams.
  typedef cmDependsFortranInternals::ObjectInfoMap ObjectInfoMap;
  ObjectInfoMap const& objInfo = this->Internal->ObjectInfo;
  for (auto const& i : objInfo) {
    if (!this->WriteDependenciesReal(i.first.c_str(), i.second, mod_dir,
                                     stamp_dir, makeDepends,
                                     internalDepends)) {
      return false;
    }
  }

  // Store the list of modules provided by this target.
  std::string fiName = this->TargetDirectory;
  fiName += "/fortran.internal";
  cmGeneratedFileStream fiStream(fiName);
  fiStream << "# The fortran modules provided by this target.\n";
  fiStream << "provides\n";
  std::set<std::string> const& provides = this->Internal->TargetProvides;
  for (std::string const& i : provides) {
    fiStream << " " << i << "\n";
  }

  // Create a script to clean the modules.
  if (!provides.empty()) {
    std::string fcName = this->TargetDirectory;
    fcName += "/cmake_clean_Fortran.cmake";
    cmGeneratedFileStream fcStream(fcName);
    fcStream << "# Remove fortran modules provided by this target.\n";
    fcStream << "FILE(REMOVE";
    std::string currentBinDir =
      this->LocalGenerator->GetCurrentBinaryDirectory();
    for (std::string const& i : provides) {
      std::string mod_upper = mod_dir;
      mod_upper += "/";
      std::string mod_lower = mod_dir;
      mod_lower += "/";
      cmFortranModuleAppendUpperLower(i, mod_upper, mod_lower);
      std::string stamp = stamp_dir;
      stamp += "/";
      stamp += i;
      stamp += ".stamp";
      fcStream << "\n";
      fcStream << "  \""
               << this->MaybeConvertToRelativePath(currentBinDir, mod_lower)
               << "\"\n";
      fcStream << "  \""
               << this->MaybeConvertToRelativePath(currentBinDir, mod_upper)
               << "\"\n";
      fcStream << "  \""
               << this->MaybeConvertToRelativePath(currentBinDir, stamp)
               << "\"\n";
    }
    fcStream << "  )\n";
  }
  return true;
}

void cmDependsFortran::LocateModules()
{
  // Collect the set of modules provided and required by all sources.
  typedef cmDependsFortranInternals::ObjectInfoMap ObjectInfoMap;
  ObjectInfoMap const& objInfo = this->Internal->ObjectInfo;
  for (auto const& infoI : objInfo) {
    cmFortranSourceInfo const& info = infoI.second;
    // Include this module in the set provided by this target.
    this->Internal->TargetProvides.insert(info.Provides.begin(),
                                          info.Provides.end());

    for (std::string const& r : info.Requires) {
      this->Internal->TargetRequires[r].clear();
    }
  }

  // Short-circuit for simple targets.
  if (this->Internal->TargetRequires.empty()) {
    return;
  }

  // Match modules provided by this target to those it requires.
  this->MatchLocalModules();

  // Load information about other targets.
  cmMakefile* mf = this->LocalGenerator->GetMakefile();
  std::vector<std::string> infoFiles;
  if (const char* infoFilesValue =
        mf->GetDefinition("CMAKE_TARGET_LINKED_INFO_FILES")) {
    cmSystemTools::ExpandListArgument(infoFilesValue, infoFiles);
  }
  for (std::string const& i : infoFiles) {
    std::string targetDir = cmSystemTools::GetFilenamePath(i);
    std::string fname = targetDir + "/fortran.internal";
    cmsys::ifstream fin(fname.c_str());
    if (fin) {
      this->MatchRemoteModules(fin, targetDir.c_str());
    }
  }
}

void cmDependsFortran::MatchLocalModules()
{
  const char* stampDir = this->TargetDirectory.c_str();
  std::set<std::string> const& provides = this->Internal->TargetProvides;
  for (std::string const& i : provides) {
    this->ConsiderModule(i.c_str(), stampDir);
  }
}

void cmDependsFortran::MatchRemoteModules(std::istream& fin,
                                          const char* stampDir)
{
  std::string line;
  bool doing_provides = false;
  while (cmSystemTools::GetLineFromStream(fin, line)) {
    // Ignore comments and empty lines.
    if (line.empty() || line[0] == '#' || line[0] == '\r') {
      continue;
    }

    if (line[0] == ' ') {
      if (doing_provides) {
        std::string mod = line;
        if (!cmHasLiteralSuffix(mod, ".mod") &&
            !cmHasLiteralSuffix(mod, ".smod")) {
          // Support fortran.internal files left by older versions of CMake.
          // They do not include the ".mod" extension.
          mod += ".mod";
        }
        this->ConsiderModule(mod.c_str() + 1, stampDir);
      }
    } else if (line == "provides") {
      doing_provides = true;
    } else {
      doing_provides = false;
    }
  }
}

void cmDependsFortran::ConsiderModule(const char* name, const char* stampDir)
{
  // Locate each required module.
  typedef cmDependsFortranInternals::TargetRequiresMap TargetRequiresMap;
  TargetRequiresMap::iterator required =
    this->Internal->TargetRequires.find(name);
  if (required != this->Internal->TargetRequires.end() &&
      required->second.empty()) {
    // The module is provided by a CMake target.  It will have a stamp file.
    std::string stampFile = stampDir;
    stampFile += "/";
    stampFile += name;
    stampFile += ".stamp";
    required->second = stampFile;
  }
}

bool cmDependsFortran::WriteDependenciesReal(const char* obj,
                                             cmFortranSourceInfo const& info,
                                             std::string const& mod_dir,
                                             const char* stamp_dir,
                                             std::ostream& makeDepends,
                                             std::ostream& internalDepends)
{
  typedef cmDependsFortranInternals::TargetRequiresMap TargetRequiresMap;

  // Get the source file for this object.
  const char* src = info.Source.c_str();

  // Write the include dependencies to the output stream.
  std::string binDir = this->LocalGenerator->GetBinaryDirectory();
  std::string obj_i = this->MaybeConvertToRelativePath(binDir, obj);
  std::string obj_m = cmSystemTools::ConvertToOutputPath(obj_i);
  internalDepends << obj_i << std::endl;
  internalDepends << " " << src << std::endl;
  for (std::string const& i : info.Includes) {
    makeDepends << obj_m << ": "
                << cmSystemTools::ConvertToOutputPath(
                     this->MaybeConvertToRelativePath(binDir, i))
                << std::endl;
    internalDepends << " " << i << std::endl;
  }
  makeDepends << std::endl;

  // Write module requirements to the output stream.
  for (std::string const& i : info.Requires) {
    // Require only modules not provided in the same source.
    if (info.Provides.find(i) != info.Provides.cend()) {
      continue;
    }

    // The object file should depend on timestamped files for the
    // modules it uses.
    TargetRequiresMap::const_iterator required =
      this->Internal->TargetRequires.find(i);
    if (required == this->Internal->TargetRequires.end()) {
      abort();
    }
    if (!required->second.empty()) {
      // This module is known.  Depend on its timestamp file.
      std::string stampFile = cmSystemTools::ConvertToOutputPath(
        this->MaybeConvertToRelativePath(binDir, required->second));
      makeDepends << obj_m << ": " << stampFile << "\n";
    } else {
      // This module is not known to CMake.  Try to locate it where
      // the compiler will and depend on that.
      std::string module;
      if (this->FindModule(i, module)) {
        module = cmSystemTools::ConvertToOutputPath(
          this->MaybeConvertToRelativePath(binDir, module));
        makeDepends << obj_m << ": " << module << "\n";
      }
    }
  }

  // If any modules are provided then they must be converted to stamp files.
  if (!info.Provides.empty()) {
    // Create a target to copy the module after the object file
    // changes.
    for (std::string const& i : info.Provides) {
      // Include this module in the set provided by this target.
      this->Internal->TargetProvides.insert(i);

      // Always use lower case for the mod stamp file name.  The
      // cmake_copy_f90_mod will call back to this class, which will
      // try various cases for the real mod file name.
      std::string modFile = mod_dir;
      modFile += "/";
      modFile += i;
      modFile = this->LocalGenerator->ConvertToOutputFormat(
        this->MaybeConvertToRelativePath(binDir, modFile),
        cmOutputConverter::SHELL);
      std::string stampFile = stamp_dir;
      stampFile += "/";
      stampFile += i;
      stampFile += ".stamp";
      stampFile = this->MaybeConvertToRelativePath(binDir, stampFile);
      std::string const stampFileForShell =
        this->LocalGenerator->ConvertToOutputFormat(stampFile,
                                                    cmOutputConverter::SHELL);
      std::string const stampFileForMake =
        cmSystemTools::ConvertToOutputPath(stampFile);

      makeDepends << obj_m << ".provides.build"
                  << ": " << stampFileForMake << "\n";
      // Note that when cmake_copy_f90_mod finds that a module file
      // and the corresponding stamp file have no differences, the stamp
      // file is not updated. In such case the stamp file will be always
      // older than its prerequisite and trigger cmake_copy_f90_mod
      // on each new build. This is expected behavior for incremental
      // builds and can not be changed without preforming recursive make
      // calls that would considerably slow down the building process.
      makeDepends << stampFileForMake << ": " << obj_m << "\n";
      makeDepends << "\t$(CMAKE_COMMAND) -E cmake_copy_f90_mod " << modFile
                  << " " << stampFileForShell;
      cmMakefile* mf = this->LocalGenerator->GetMakefile();
      const char* cid = mf->GetDefinition("CMAKE_Fortran_COMPILER_ID");
      if (cid && *cid) {
        makeDepends << " " << cid;
      }
      makeDepends << "\n";
    }
    makeDepends << obj_m << ".provides.build:\n";
    // After copying the modules update the timestamp file.
    makeDepends << "\t$(CMAKE_COMMAND) -E touch " << obj_m
                << ".provides.build\n";

    // Make sure the module timestamp rule is evaluated by the time
    // the target finishes building.
    std::string driver = this->TargetDirectory;
    driver += "/build";
    driver = cmSystemTools::ConvertToOutputPath(
      this->MaybeConvertToRelativePath(binDir, driver));
    makeDepends << driver << ": " << obj_m << ".provides.build\n";
  }

  return true;
}

bool cmDependsFortran::FindModule(std::string const& name, std::string& module)
{
  // Construct possible names for the module file.
  std::string mod_upper;
  std::string mod_lower;
  cmFortranModuleAppendUpperLower(name, mod_upper, mod_lower);

  // Search the include path for the module.
  std::string fullName;
  for (std::string const& ip : this->IncludePath) {
    // Try the lower-case name.
    fullName = ip;
    fullName += "/";
    fullName += mod_lower;
    if (cmSystemTools::FileExists(fullName, true)) {
      module = fullName;
      return true;
    }

    // Try the upper-case name.
    fullName = ip;
    fullName += "/";
    fullName += mod_upper;
    if (cmSystemTools::FileExists(fullName, true)) {
      module = fullName;
      return true;
    }
  }
  return false;
}

bool cmDependsFortran::CopyModule(const std::vector<std::string>& args)
{
  // Implements
  //
  //   $(CMAKE_COMMAND) -E cmake_copy_f90_mod input.mod output.mod.stamp
  //                                          [compiler-id]
  //
  // Note that the case of the .mod file depends on the compiler.  In
  // the future this copy could also account for the fact that some
  // compilers include a timestamp in the .mod file so it changes even
  // when the interface described in the module does not.

  std::string mod = args[2];
  std::string stamp = args[3];
  std::string compilerId;
  if (args.size() >= 5) {
    compilerId = args[4];
  }
  if (!cmHasLiteralSuffix(mod, ".mod") && !cmHasLiteralSuffix(mod, ".smod")) {
    // Support depend.make files left by older versions of CMake.
    // They do not include the ".mod" extension.
    mod += ".mod";
  }
  std::string mod_dir = cmSystemTools::GetFilenamePath(mod);
  if (!mod_dir.empty()) {
    mod_dir += "/";
  }
  std::string mod_upper = mod_dir;
  std::string mod_lower = mod_dir;
  cmFortranModuleAppendUpperLower(cmSystemTools::GetFilenameName(mod),
                                  mod_upper, mod_lower);
  if (cmSystemTools::FileExists(mod_upper, true)) {
    if (cmDependsFortran::ModulesDiffer(mod_upper.c_str(), stamp.c_str(),
                                        compilerId.c_str())) {
      if (!cmSystemTools::CopyFileAlways(mod_upper, stamp)) {
        std::cerr << "Error copying Fortran module from \"" << mod_upper
                  << "\" to \"" << stamp << "\".\n";
        return false;
      }
    }
    return true;
  }
  if (cmSystemTools::FileExists(mod_lower, true)) {
    if (cmDependsFortran::ModulesDiffer(mod_lower.c_str(), stamp.c_str(),
                                        compilerId.c_str())) {
      if (!cmSystemTools::CopyFileAlways(mod_lower, stamp)) {
        std::cerr << "Error copying Fortran module from \"" << mod_lower
                  << "\" to \"" << stamp << "\".\n";
        return false;
      }
    }
    return true;
  }

  std::cerr << "Error copying Fortran module \"" << args[2] << "\".  Tried \""
            << mod_upper << "\" and \"" << mod_lower << "\".\n";
  return false;
}

// Helper function to look for a short sequence in a stream.  If this
// is later used for longer sequences it should be re-written using an
// efficient string search algorithm such as Boyer-Moore.
static bool cmFortranStreamContainsSequence(std::istream& ifs, const char* seq,
                                            int len)
{
  assert(len > 0);

  int cur = 0;
  while (cur < len) {
    // Get the next character.
    int token = ifs.get();
    if (!ifs) {
      return false;
    }

    // Check the character.
    if (token == static_cast<int>(seq[cur])) {
      ++cur;
    } else {
      // Assume the sequence has no repeating subsequence.
      cur = 0;
    }
  }

  // The entire sequence was matched.
  return true;
}

// Helper function to compare the remaining content in two streams.
static bool cmFortranStreamsDiffer(std::istream& ifs1, std::istream& ifs2)
{
  // Compare the remaining content.
  for (;;) {
    int ifs1_c = ifs1.get();
    int ifs2_c = ifs2.get();
    if (!ifs1 && !ifs2) {
      // We have reached the end of both streams simultaneously.
      // The streams are identical.
      return false;
    }

    if (!ifs1 || !ifs2 || ifs1_c != ifs2_c) {
      // We have reached the end of one stream before the other or
      // found differing content.  The streams are different.
      break;
    }
  }

  return true;
}

bool cmDependsFortran::ModulesDiffer(const char* modFile,
                                     const char* stampFile,
                                     const char* compilerId)
{
  /*
  gnu >= 4.9:
    A mod file is an ascii file compressed with gzip.
    Compiling twice produces identical modules.

  gnu < 4.9:
    A mod file is an ascii file.
    <bar.mod>
    FORTRAN module created from /path/to/foo.f90 on Sun Dec 30 22:47:58 2007
    If you edit this, you'll get what you deserve.
    ...
    </bar.mod>
    As you can see the first line contains the date.

  intel:
    A mod file is a binary file.
    However, looking into both generated bar.mod files with a hex editor
    shows that they differ only before a sequence linefeed-zero (0x0A 0x00)
    which is located some bytes in front of the absolute path to the source
    file.

  sun:
    A mod file is a binary file.  Compiling twice produces identical modules.

  others:
    TODO ...
  */

  /* Compilers which do _not_ produce different mod content when the same
   * source is compiled twice
   *   -SunPro
   */
  if (strcmp(compilerId, "SunPro") == 0) {
    return cmSystemTools::FilesDiffer(modFile, stampFile);
  }

#if defined(_WIN32) || defined(__CYGWIN__)
  cmsys::ifstream finModFile(modFile, std::ios::in | std::ios::binary);
  cmsys::ifstream finStampFile(stampFile, std::ios::in | std::ios::binary);
#else
  cmsys::ifstream finModFile(modFile);
  cmsys::ifstream finStampFile(stampFile);
#endif
  if (!finModFile || !finStampFile) {
    // At least one of the files does not exist.  The modules differ.
    return true;
  }

  /* Compilers which _do_ produce different mod content when the same
   * source is compiled twice
   *   -GNU
   *   -Intel
   *
   * Eat the stream content until all recompile only related changes
   * are left behind.
   */
  if (strcmp(compilerId, "GNU") == 0) {
    // GNU Fortran 4.9 and later compress .mod files with gzip
    // but also do not include a date so we can fall through to
    // compare them without skipping any prefix.
    unsigned char hdr[2];
    bool okay = !finModFile.read(reinterpret_cast<char*>(hdr), 2).fail();
    finModFile.seekg(0);
    if (!okay || hdr[0] != 0x1f || hdr[1] != 0x8b) {
      const char seq[1] = { '\n' };
      const int seqlen = 1;

      if (!cmFortranStreamContainsSequence(finModFile, seq, seqlen)) {
        // The module is of unexpected format.  Assume it is different.
        std::cerr << compilerId << " fortran module " << modFile
                  << " has unexpected format." << std::endl;
        return true;
      }

      if (!cmFortranStreamContainsSequence(finStampFile, seq, seqlen)) {
        // The stamp must differ if the sequence is not contained.
        return true;
      }
    }
  } else if (strcmp(compilerId, "Intel") == 0) {
    const char seq[2] = { '\n', '\0' };
    const int seqlen = 2;

    // Skip the leading byte which appears to be a version number.
    // We do not need to check for an error because the sequence search
    // below will fail in that case.
    finModFile.get();
    finStampFile.get();

    if (!cmFortranStreamContainsSequence(finModFile, seq, seqlen)) {
      // The module is of unexpected format.  Assume it is different.
      std::cerr << compilerId << " fortran module " << modFile
                << " has unexpected format." << std::endl;
      return true;
    }

    if (!cmFortranStreamContainsSequence(finStampFile, seq, seqlen)) {
      // The stamp must differ if the sequence is not contained.
      return true;
    }
  }

  // Compare the remaining content.  If no compiler id matched above,
  // including the case none was given, this will compare the whole
  // content.
  return cmFortranStreamsDiffer(finModFile, finStampFile);
}

std::string cmDependsFortran::MaybeConvertToRelativePath(
  std::string const& base, std::string const& path)
{
  if (!cmOutputConverter::ContainedInDirectory(
        base, path, this->LocalGenerator->GetStateSnapshot().GetDirectory())) {
    return path;
  }
  return cmSystemTools::ForceToRelativePath(base, path);
}
