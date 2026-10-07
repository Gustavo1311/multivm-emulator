package com.multivm.core;

/** Um drive da VM e a midia que esta nele (ver {@link VirtualMachine#getMediaSlots()}). */
public final class MediaSlot {
    public enum Kind { HARD_DISK, CDROM, FLOPPY }

    public enum Bus { IDE, SATA, FLOPPY, VIRTIO }

    public final int slot;
    public final Kind kind;
    public final Bus bus;
    /** Unidade: canal*2+mestre/escravo no IDE, porta no SATA, 0 = A: e 1 = B: no disquete. */
    public final int unit;
    public final boolean present;
    public final boolean readOnly;
    /** A midia pode ser trocada/removida com a VM ligada. */
    public final boolean changeable;
    public final String name;

    private MediaSlot(int slot, Kind kind, Bus bus, int unit, boolean present, boolean readOnly,
                      boolean changeable, String name) {
        this.slot = slot;
        this.kind = kind;
        this.bus = bus;
        this.unit = unit;
        this.present = present;
        this.readOnly = readOnly;
        this.changeable = changeable;
        this.name = name;
    }

    static MediaSlot parse(int slot, String s) {
        String[] f = s.split("\\|", 7);
        return new MediaSlot(slot, Kind.values()[Integer.parseInt(f[0])], Bus.values()[Integer.parseInt(f[1])],
                Integer.parseInt(f[2]), f[3].equals("1"), f[4].equals("1"), f[5].equals("1"),
                f.length > 6 ? f[6] : "");
    }

    @Override
    public String toString() {
        return slot + ": " + kind + "/" + bus + " unidade " + unit + (present ? " [" + name + "]" : " (vazio)");
    }
}
