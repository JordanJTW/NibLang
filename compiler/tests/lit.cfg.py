import os

import lit.formats

config.name = 'NibLang'
config.test_format = lit.formats.ShTest()
config.use_default_shell = True  # Allows `|` even if %compiler returns 1

config.suffixes = ['.nib']

config.test_source_root = os.path.dirname(__file__)
project_root = os.path.abspath(os.path.join(config.test_source_root, "../.."))

obj_root = getattr(config, 'my_obj_root', None)
if not obj_root:
    obj_root = os.path.join(project_root, 'build')

src_root = getattr(config, 'my_src_root', None)
if not src_root:
    src_root = project_root

config.test_exec_root = os.path.join(obj_root, 'test')
config.environment['NIB_PATH'] = os.path.join(src_root, "compiler");
config.substitutions.append(('%compiler', os.path.join(obj_root, 'compiler/compiler')))
config.substitutions.append(('%filecheck', os.path.join(obj_root, 'venv/bin/filecheck')))
config.substitutions.append(('%run', os.path.join(obj_root, 'src/run')))
config.substitutions.append(('%root', src_root))