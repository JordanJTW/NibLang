import lit.formats

config.name = 'NibLang'
config.test_format = lit.formats.ShTest(execute_external=True)
config.use_default_shell = True  # Allows `|` even if %compiler returns 1

config.suffixes = ['.nib']

config.test_source_root = os.path.dirname(__file__)
config.test_exec_root = os.path.join(config.my_obj_root, 'test')

config.environment['NIB_PATH'] = os.path.join(config.my_src_root, "compiler");
config.substitutions.append(('%compiler', os.path.join(config.my_obj_root, 'compiler/compiler')))
config.substitutions.append(('%run', os.path.join(config.my_obj_root, 'src/run')))