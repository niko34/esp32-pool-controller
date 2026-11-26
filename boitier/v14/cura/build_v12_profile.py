"""Build and statically check a Cura profile. No printer/user/CAD files edited."""
import configparser
import hashlib
import json
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parent
DEFS = Path('/Applications/UltiMaker Cura.app/Contents/Resources/share/cura/resources/definitions')
OUTPUT = ROOT / 'PoolController_V12_Capot_SUNLU_PLAPlus_Genius_0.4_Supports.curaprofile'
NAMES = ('poolcontroller_v12_capot_pla_plus_supports_global',
         'artillery_base_extruder_0_poolcontroller_v12_capot_pla_plus_supports')
OLD = ROOT / 'PoolController_SUNLU_PLAPlus_Genius_0.4.curaprofile'
OLD_HASH = '25bbd173a2fb47b3c41268832391ed9624265e555ad27ac7aba6474098fb5160'


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def parse(data):
    cfg = configparser.ConfigParser(interpolation=None, strict=True)
    cfg.read_string(data.decode('utf-8'))
    require(set(cfg.sections()) == {'general', 'metadata', 'values'}, 'Invalid sections')
    return cfg


def main():
    definition = json.loads((DEFS / 'fdmprinter.def.json').read_text())
    settings = {}

    def flatten(nodes):
        for key, val in nodes.items():
            settings[key] = val
            flatten(val.get('children', {}))

    flatten(definition['settings'])
    printer = json.loads((DEFS / 'artillery_genius.def.json').read_text())
    require(printer['inherits'] == 'artillery_base', 'Unexpected printer inheritance')
    require(digest(OLD) == OLD_HASH, 'Historical profile changed')
    payloads = {name: (ROOT / 'v12_sources' / name).read_bytes() for name in NAMES}
    configs = [parse(payloads[name]) for name in NAMES]
    for index, cfg in enumerate(configs):
        require(cfg['general']['version'] == '4', 'Wrong container version')
        require(cfg['general']['definition'] == 'artillery_base', 'Wrong definition')
        require(cfg['metadata']['type'] == 'quality_changes', 'Wrong profile type')
        require(cfg['metadata']['quality_type'] == 'standard', 'Wrong quality type')
        require(int(cfg['metadata']['setting_version']) == definition['metadata']['setting_version'], 'Wrong setting version')
        require(cfg['metadata'].get('position') == ('0' if index else None), 'Wrong extruder metadata')
        for key, raw in cfg['values'].items():
            require(key in settings, f'Unknown setting: {key}')
            setting = settings[key]
            kind = setting['type']
            if kind == 'enum':
                require(raw in setting['options'], f'Invalid enum: {key}={raw}')
            elif kind == 'bool':
                require(raw in ('True', 'False'), f'Invalid bool: {key}')
            elif kind in ('int', 'float'):
                value = int(raw) if kind == 'int' else float(raw)
                require(value == value and abs(value) != float('inf'), f'Non-finite value: {key}')
            if index:
                require(setting.get('settable_per_extruder', True), f'Global-only setting: {key}')
    require(configs[0]['general']['name'] == configs[1]['general']['name'], 'Names differ')
    combined = {**configs[0]['values'], **configs[1]['values']}
    expected = {'support_enable': 'True', 'support_structure': 'normal', 'support_type': 'everywhere',
                'support_angle': '50', 'support_infill_rate': '12', 'support_interface_enable': 'True',
                'support_xy_overrides_z': 'z_overrides_xy', 'support_z_distance': '0.2',
                'support_top_distance': '0.2', 'support_bottom_distance': '0.2', 'support_xy_distance': '0.6',
                'layer_height': '0.2', 'material_print_temperature': '210', 'wall_line_count': '4'}
    require(all(combined.get(k) == v for k, v in expected.items()), 'Critical settings mismatch')
    old_cfg = parse((ROOT / 'artillery_base_extruder_0_poolcontroller_sunlu_pla_plus_genius').read_bytes())
    require(all(combined.get(k) == v for k, v in old_cfg['values'].items()), 'Unintended base-profile change')
    if OUTPUT.exists():
        with zipfile.ZipFile(OUTPUT) as archive:
            require(set(archive.namelist()) == set(NAMES), 'Unexpected archive entries')
            require(all(archive.read(k) == v for k, v in payloads.items()), 'Output differs: choose a new revision filename')
    else:
        with zipfile.ZipFile(OUTPUT, 'x', compression=zipfile.ZIP_DEFLATED) as archive:
            for name, payload in payloads.items():
                entry = zipfile.ZipInfo(name, date_time=(2026, 10, 7, 12, 0, 0))
                entry.compress_type = zipfile.ZIP_DEFLATED
                archive.writestr(entry, payload)
    with zipfile.ZipFile(OUTPUT) as archive:
        require(archive.testzip() is None, 'ZIP integrity failure')
        require(all(archive.read(k) == v for k, v in payloads.items()), 'Export round-trip failed')
    require(digest(OLD) == OLD_HASH, 'Historical profile modified')
    print(json.dumps({'static_validation_pass': True, 'profile': str(OUTPUT),
                      'sha256': digest(OUTPUT), 'settings_checked': sum(len(c['values']) for c in configs),
                      'cura_setting_version': definition['metadata']['setting_version'],
                      'reference_definition': str(DEFS / 'fdmprinter.def.json'),
                      'historical_profile_unchanged': True, 'gui_import_tested': False,
                      'slicing_tested': False, 'physical_print_tested': False}, indent=2))


if __name__ == '__main__':
    main()
