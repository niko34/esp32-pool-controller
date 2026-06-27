#  Force --coverage sur l'étape d'édition de liens (env native_coverage).
#  Sans ça, les symboles runtime gcov (llvm_gcda_*, llvm_gcov_init) émis par les
#  objets instrumentés ne sont pas résolus → "symbol(s) not found".
#  Compilation : --coverage est dans build_flags (platformio.ini).
Import("env")  # noqa: F821

env.Append(LINKFLAGS=["--coverage"])
