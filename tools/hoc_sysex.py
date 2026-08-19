#!/usr/bin/env python3
"""hOC Captain MIDI SysEx configuration tool.

Reference implementation of docs/hoc-midi-sysex.md, suitable for driving
the hOC from a host (e.g. Jetson Orin) and for protocol conformance
testing.

    pip install mido python-rtmidi

    hoc_sysex.py ports                 # list MIDI ports
    hoc_sysex.py identity              # universal device inquiry
    hoc_sysex.py info
    hoc_sysex.py get <class> <idx> <param>
    hoc_sysex.py set <class> <idx> <param> <value>
    hoc_sysex.py dump [setup#] [-o file.json]
    hoc_sysex.py restore file.json     # replay a dump to the live config
    hoc_sysex.py save                  # commit to EEPROM
    hoc_sysex.py load <setup#>         # revert to stored setup
    hoc_sysex.py select <setup#>       # switch setup, keeping edits
    hoc_sysex.py factory               # factory reset (asks for confirmation)
    hoc_sysex.py panic
    hoc_sysex.py monitor               # print incoming SysEx
"""
import argparse
import json
import sys
import time

try:
    import mido
except ImportError:
    sys.exit("mido is required: pip install mido python-rtmidi")

PROTO_VERSION = 1
HEADER = [0x7D, 0x62, 0x4D, PROTO_VERSION]  # after F0 (mido adds F0/F7)

CMD_INFO, CMD_GET, CMD_SET, CMD_GET_DUMP = 0x01, 0x02, 0x03, 0x04
CMD_SAVE, CMD_LOAD, CMD_FACTORY, CMD_PANIC, CMD_SELECT = 0x06, 0x07, 0x08, 0x09, 0x0A
CMD_ACK, CMD_INFO_R, CMD_GET_R = 0x40, 0x41, 0x42
CMD_DUMP_DATA, CMD_DUMP_END, CMD_NAK = 0x44, 0x45, 0x7E

NAK_ERRORS = {
    1: "protocol version mismatch",
    2: "unknown command",
    3: "bad address",
    4: "bad value / read-only",
    5: "busy",
    6: "dump checksum mismatch",
}

CLASS_NAMES = {0: "global", 1: "in", 2: "cv", 3: "tr"}
CLASS_IDS = {v: k for k, v in CLASS_NAMES.items()}
PARAM_COUNT = {0: 7, 1: 7, 2: 9, 3: 9}


def find_port(substr):
    # comma-separated alternatives; the hOC enumerates as USB device
    # "Phazerville" but its MIDI port is named "PewPewMIDI"
    names = mido.get_output_names()
    for alt in substr.split(","):
        for n in names:
            if alt.strip().lower() in n.lower():
                return n
    sys.exit(f"no MIDI port matching {substr!r}; available: {names}")


class HocDevice:
    def __init__(self, port_substr="Phazerville", timeout=2.0):
        name = find_port(port_substr)
        self.out = mido.open_output(name)
        self.inp = mido.open_input(name)
        self.timeout = timeout

    def send_cmd(self, cmd, payload=()):
        self.out.send(mido.Message("sysex", data=HEADER + [cmd] + list(payload)))

    def recv_sysex(self, timeout=None):
        deadline = time.time() + (timeout or self.timeout)
        while time.time() < deadline:
            for msg in self.inp.iter_pending():
                if msg.type == "sysex":
                    return list(msg.data)
            time.sleep(0.005)
        return None

    def recv_reply(self):
        """Wait for a protocol frame; returns (cmd, payload) or exits on NAK."""
        while True:
            data = self.recv_sysex()
            if data is None:
                sys.exit("timeout waiting for reply")
            if data[:4] != HEADER:
                continue  # not ours (device inquiry replies handled separately)
            cmd, payload = data[4], data[5:]
            if cmd == CMD_NAK:
                err = NAK_ERRORS.get(payload[1] if len(payload) > 1 else 0, "?")
                sys.exit(f"NAK for command 0x{payload[0]:02X}: {err}")
            return cmd, payload

    def transact(self, cmd, payload=(), expect=CMD_ACK):
        self.send_cmd(cmd, payload)
        rcmd, rpayload = self.recv_reply()
        if rcmd != expect:
            sys.exit(f"unexpected reply 0x{rcmd:02X} (wanted 0x{expect:02X}): {rpayload}")
        return rpayload


def cmd_ports(_):
    print("outputs:", *mido.get_output_names(), sep="\n  ")


def cmd_identity(args):
    dev = HocDevice(args.port)
    dev.out.send(mido.Message("sysex", data=[0x7E, 0x7F, 0x06, 0x01]))
    data = dev.recv_sysex()
    if data is None:
        sys.exit("no identity reply")
    if len(data) >= 12 and data[0] == 0x7E and data[3] == 0x02:
        family = bytes(data[5:8]).decode(errors="replace")
        print(f"manufacturer 0x{data[4]:02X}, family {family!r}, "
              f"protocol v{data[8]}, firmware {data[9]}.{data[10]}.{data[11]}")
    else:
        print("identity reply:", [hex(b) for b in data])


def cmd_info(args):
    dev = HocDevice(args.port)
    p = dev.transact(CMD_INFO, expect=CMD_INFO_R)
    keys = ["schema", "fw_major", "fw_minor", "fw_patch", "n_inmaps",
            "n_cv_out", "n_trig_out", "n_setups", "active_setup", "dirty"]
    info = dict(zip(keys, p))
    print(json.dumps(info, indent=2))
    return info


def parse_class(s):
    if s in CLASS_IDS:
        return CLASS_IDS[s]
    return int(s)


def cmd_get(args):
    dev = HocDevice(args.port)
    cls = parse_class(args.cls)
    p = dev.transact(CMD_GET, [cls, args.idx, args.param], expect=CMD_GET_R)
    print(f"{CLASS_NAMES.get(p[0], p[0])}[{p[1]}].{p[2]} = {p[3]}")


def cmd_set(args):
    dev = HocDevice(args.port)
    cls = parse_class(args.cls)
    dev.transact(CMD_SET, [cls, args.idx, args.param, args.value])
    print("ok")


def parse_dump_records(payload):
    """DUMP_DATA payload -> (setup, seq, total, [(cls, idx, [values])])"""
    setup, seq, total = payload[0], payload[1], payload[2]
    records, i = [], 3
    while i + 1 < len(payload):
        cls, idx = payload[i], payload[i + 1]
        n = PARAM_COUNT.get(cls)
        if n is None or i + 2 + n > len(payload):
            break
        records.append((cls, idx, payload[i + 2:i + 2 + n]))
        i += 2 + n
    return setup, seq, total, records


def cmd_dump(args):
    dev = HocDevice(args.port)
    setup = 0x7F if args.setup is None else args.setup
    dev.send_cmd(CMD_GET_DUMP, [setup])
    records, packets, xor = [], 0, 0
    while True:
        cmd, payload = dev.recv_reply()
        if cmd == CMD_DUMP_DATA:
            s, seq, total, recs = parse_dump_records(payload)
            records.extend(recs)
            packets += 1
            for cls, idx, vals in recs:
                xor ^= cls ^ idx
                for v in vals:
                    xor ^= v
        elif cmd == CMD_DUMP_END:
            if payload[1] != packets or payload[2] != (xor & 0x7F):
                sys.exit(f"dump checksum mismatch: {payload[1]}/{packets} "
                         f"packets, xor {payload[2]}/{xor & 0x7F}")
            break
        else:
            sys.exit(f"unexpected 0x{cmd:02X} during dump")
    out = {
        "setup": records and payload[0],
        "records": [
            {"class": CLASS_NAMES.get(cls, cls), "idx": idx, "values": list(vals)}
            for cls, idx, vals in records
        ],
    }
    text = json.dumps(out, indent=2)
    if args.output:
        with open(args.output, "w") as f:
            f.write(text + "\n")
        print(f"wrote {len(records)} records to {args.output}")
    else:
        print(text)


def cmd_restore(args):
    dev = HocDevice(args.port)
    with open(args.file) as f:
        doc = json.load(f)
    recs = [(CLASS_IDS.get(r["class"], r["class"]), r["idx"], r["values"])
            for r in doc["records"]]
    # chunk whole records into <=49-byte payload packets, mirroring the device
    packets, cur = [], []
    for cls, idx, vals in recs:
        rec = [cls, idx] + list(vals)
        if sum(len(r) for r in cur) + len(rec) > 49:
            packets.append(cur)
            cur = []
        cur.append(rec)
    if cur:
        packets.append(cur)
    xor = 0
    setup = doc.get("setup") or 0
    for seq, pkt in enumerate(packets):
        flat = [b for rec in pkt for b in rec]
        for b in flat:
            xor ^= b
        payload = dev.transact(CMD_DUMP_DATA, [setup, seq, len(packets)] + flat)
        if payload and payload[0] != seq:
            sys.exit(f"packet {seq} acked out of order: {payload}")
    dev.transact(CMD_DUMP_END, [setup, len(packets), xor & 0x7F])
    print(f"restored {len(recs)} records ({len(packets)} packets)")


def cmd_save(args):
    HocDevice(args.port).transact(CMD_SAVE)
    print("saved")


def cmd_load(args):
    HocDevice(args.port).transact(CMD_LOAD, [args.setup])
    print(f"loaded setup {args.setup}")


def cmd_select(args):
    HocDevice(args.port).transact(CMD_SELECT, [args.setup])
    print(f"selected setup {args.setup}")


def cmd_factory(args):
    if not args.yes:
        if input("factory reset ALL setups? [y/N] ").strip().lower() != "y":
            sys.exit("aborted")
    HocDevice(args.port).transact(CMD_FACTORY, [0x21, 0x42])
    print("factory reset done")


def cmd_panic(args):
    HocDevice(args.port).transact(CMD_PANIC)
    print("panic sent")


def cmd_monitor(args):
    dev = HocDevice(args.port)
    print("monitoring SysEx (ctrl-C to stop)...")
    try:
        while True:
            data = dev.recv_sysex(timeout=3600)
            if data:
                print(" ".join(f"{b:02X}" for b in data))
    except KeyboardInterrupt:
        pass


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="PewPewMIDI,Phazerville",
                    help="comma-separated port name substrings tried in order")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("ports").set_defaults(fn=cmd_ports)
    sub.add_parser("identity").set_defaults(fn=cmd_identity)
    sub.add_parser("info").set_defaults(fn=cmd_info)

    g = sub.add_parser("get")
    g.add_argument("cls", help="global|in|cv|tr or numeric class")
    g.add_argument("idx", type=int)
    g.add_argument("param", type=int)
    g.set_defaults(fn=cmd_get)

    s = sub.add_parser("set")
    s.add_argument("cls", help="global|in|cv|tr or numeric class")
    s.add_argument("idx", type=int)
    s.add_argument("param", type=int)
    s.add_argument("value", type=int)
    s.set_defaults(fn=cmd_set)

    d = sub.add_parser("dump")
    d.add_argument("setup", nargs="?", type=int, default=None)
    d.add_argument("-o", "--output")
    d.set_defaults(fn=cmd_dump)

    r = sub.add_parser("restore")
    r.add_argument("file")
    r.set_defaults(fn=cmd_restore)

    sub.add_parser("save").set_defaults(fn=cmd_save)

    l = sub.add_parser("load")
    l.add_argument("setup", type=int)
    l.set_defaults(fn=cmd_load)

    se = sub.add_parser("select")
    se.add_argument("setup", type=int)
    se.set_defaults(fn=cmd_select)

    f = sub.add_parser("factory")
    f.add_argument("-y", "--yes", action="store_true")
    f.set_defaults(fn=cmd_factory)

    sub.add_parser("panic").set_defaults(fn=cmd_panic)
    sub.add_parser("monitor").set_defaults(fn=cmd_monitor)

    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
