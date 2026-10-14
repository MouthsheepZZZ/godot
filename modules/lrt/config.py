def can_build(env, platform):
    return not env["disable_3d"]


def configure(env):
    pass


def get_opts(platform):
    from SCons.Variables import BoolVariable

    return [BoolVariable("lrt_hddagi", "Build the optional HDDAGI diffuse GI adapter", True)]


def get_doc_classes():
    return ["LRTVolume", "LRTVolume3D", "LRTExportPlugin"]


def get_doc_path():
    return "doc_classes"
