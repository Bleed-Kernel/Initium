from harness import fdt
from harness.api import check
from harness.ports import ALL_PORTS
from cases._common import boot_entry, check_report_clean, entry, panics_in_turn
from cases.limine_protocol import boot_variant

# Device trees given in the configuration (CONFIG.md's dtb_path and
# global_dtb) or by the firmware when there is no ACPI, as the Limine
# protocol's DTB feature passes them on (PROTOCOL.md: memory nodes removed)
# and as Initium itself reads them for the CPUs to start.

def tree(t, marker, cpus=None, **extra):
    """A device tree with a marker, two memory nodes for the bootloader to
    remove, and the /cpus node cpus gives, if any."""
    root = {'#address-cells': 2, '#size-cells': 2, 'compatible': 'limine,test', 'model': 'limine test tree',
            'limine-test-marker': marker,
            'memory@40000000': {'device_type': 'memory', 'reg': (0, 0x40000000, 0, 0x10000000)},
            'memory@80000000': {'device_type': 'memory', 'reg': (0, 0x80000000, 0, 0x10000000)}}
    if cpus:
        root['cpus'] = cpus
    root.update(extra)
    return fdt.blob(root)


def boot_dtb(t, dtb, variant='full', globals_='timeout: 0\n', smp=1, machine=None, cpu=None):
    m, _ = boot_variant(t, variant, extra_entry='dtb_path: boot():/boot/test.dtb', globals_=globals_, smp=smp,
                        machine=machine, cpu=cpu, files={'/boot/test.dtb': dtb})
    r = t.kernel_report(m)
    check_report_clean(t, r)
    m.stop()
    return r


@check(ports=ALL_PORTS, tags=['limine-protocol', 'dtb'])
def dtb_path_response(t):
    """dtb_path: the executable gets that tree, firmware or no firmware tree,
    its memory nodes taken out [spec]."""
    r = boot_dtb(t, tree(t, 'from dtb_path'))
    t.expect_eq(r.int('dtb.response'), 1, 'DTB response')
    t.expect_eq(r['dtb.marker'], 'from dtb_path', 'the tree dtb_path names')
    t.expect_eq(r.int('dtb.memory_nodes'), 0, 'memory nodes removed')


@check(ports=ALL_PORTS, tags=['limine-protocol', 'dtb'])
def global_dtb(t):
    """global_dtb stands for the firmware's tree for entries without a
    dtb_path of their own, which wins where given [spec]."""
    files = {'/boot/global.dtb': tree(t, 'global'), '/boot/test.dtb': tree(t, 'entry')}
    for extra, want in (('', 'global'), ('dtb_path: boot():/boot/test.dtb', 'entry')):
        m, _ = boot_variant(t, 'full', extra_entry=extra, files=files,
                            globals_='timeout: 0\nglobal_dtb: boot():/boot/global.dtb\n')
        r = t.kernel_report(m)
        check_report_clean(t, r)
        t.expect_eq(r['dtb.marker'], want, f'{extra or "no dtb_path"}: the tree used')
        m.stop()


@check(ports=ALL_PORTS, tags=['limine-protocol', 'dtb', 'hostile'])
def dtb_path_errors(t):
    """A dtb_path that does not open, or holds no valid tree, is refused, and
    the menu comes back [intent]."""
    good = tree(t, 'x')
    cases = [
        ('missing', None, 'dtb: Failed to open device tree blob with path `boot():/boot/missing.dtb`. '
                          'Is the path correct?'),
        ('not a tree', b'\0' * 256, "dtb: Invalid device tree blob at `boot():/boot/d1.dtb`: 'FDT_ERR_BADMAGIC'"),
        ('cut short', good[:len(good) // 2],
         "dtb: Invalid device tree blob at `boot():/boot/d2.dtb`: 'FDT_ERR_TRUNCATED'"),
        ('wrong version', good[:20] + (1).to_bytes(4, 'big') + good[24:],
         "dtb: Invalid device tree blob at `boot():/boot/d3.dtb`: 'FDT_ERR_BADVERSION'"),
    ]
    files, entries = {}, ''
    for i, (_, data, _) in enumerate(cases):
        path = 'boot():/boot/missing.dtb' if data is None else f'boot():/boot/d{i}.dtb'
        if data is not None:
            files[f'/boot/d{i}.dtb'] = data
        entries += entry(f'D{i}', path='boot():/boot/kernel.elf', extra=f'dtb_path: {path}')
    m, _ = boot_entry(t, 'timeout: no\n' + entries, files=files)
    msgs = panics_in_turn(t, m, [f'D{i}' for i in range(len(cases))])
    for (what, _, want), got in zip(cases, msgs):
        t.check(got == want, f'{what}: {got!r}, want {want!r}')
