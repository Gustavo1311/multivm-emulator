package com.multivm.core;

import android.os.ParcelFileDescriptor;

/**
 * Imagem de disco. O formato e reconhecido sozinho: raw/ISO, qcow2, VDI, VMDK, VHD,
 * VHDX ou MVD (formato proprio, ver {@link DiskImages}). Pode vir de um caminho ou de
 * um descritor obtido pelo Storage Access Framework (imagens que dependem de outros
 * arquivos -- arquivo base, extents de VMDK -- precisam ser abertas pelo caminho).
 *
 * <p>O {@link Type} define como o convidado enxerga o disco. Em {@link Type#AUTO}
 * a VM usa virtio-blk (/dev/vda, ...) no boot direto de kernel e IDE quando
 * a maquina inicia por BIOS, para que o firmware consiga dar boot.
 */
public final class DiskImage {

    /** Tipo de controlador (valores iguais a mvm_disk_type). */
    public enum Type {
        AUTO(0), VIRTIO(1), IDE(2), CDROM(3),
        /** Disco SATA no controlador AHCI (x86). */
        SATA(4),
        /** CD/DVD-ROM ATAPI no controlador AHCI (x86). */
        SATA_CDROM(5),
        /** Disquete no controlador 82078 (x86, drives A: e B:). */
        FLOPPY(6);

        final int nativeId;

        Type(int id) { nativeId = id; }
    }

    final String path;
    final ParcelFileDescriptor pfd;
    final boolean readOnly;
    final Type type;
    String name;

    private DiskImage(String path, ParcelFileDescriptor pfd, boolean readOnly, Type type) {
        this.path = path;
        this.pfd = pfd;
        this.readOnly = readOnly;
        this.type = type;
    }

    /**
     * Drive de CD sem midia ({@link Type#CDROM} ou {@link Type#SATA_CDROM}), para inserir
     * uma ISO com a VM ligada ({@link VirtualMachine#insertMedia}).
     */
    public static DiskImage emptyCdrom(Type type) {
        if (type != Type.CDROM && type != Type.SATA_CDROM) throw new IllegalArgumentException("tipo de CD");
        return new DiskImage(null, null, true, type);
    }

    /** Drive de disquete sem midia. */
    public static DiskImage emptyFloppy() {
        return new DiskImage(null, null, false, Type.FLOPPY);
    }

    /** Nome mostrado na lista de midias (por exemplo o nome do arquivo). */
    public DiskImage withName(String name) {
        this.name = name;
        return this;
    }

    public String getName() { return name; }

    public boolean isEmpty() { return path == null && pfd == null; }

    public static DiskImage fromPath(String path, boolean readOnly) {
        return fromPath(path, readOnly, Type.AUTO);
    }

    public static DiskImage fromPath(String path, boolean readOnly, Type type) {
        if (path == null) throw new NullPointerException("path");
        if (type == null) throw new NullPointerException("type");
        return new DiskImage(path, null, readOnly || type == Type.CDROM || type == Type.SATA_CDROM, type);
    }

    /**
     * Usa um descritor ja aberto (por exemplo de ContentResolver.openFileDescriptor).
     * O descritor e duplicado; o chamador continua dono do original.
     */
    public static DiskImage fromFileDescriptor(ParcelFileDescriptor pfd, boolean readOnly) {
        return fromFileDescriptor(pfd, readOnly, Type.AUTO);
    }

    public static DiskImage fromFileDescriptor(ParcelFileDescriptor pfd, boolean readOnly, Type type) {
        if (pfd == null) throw new NullPointerException("pfd");
        if (type == null) throw new NullPointerException("type");
        return new DiskImage(null, pfd, readOnly || type == Type.CDROM || type == Type.SATA_CDROM, type);
    }

    /** Imagem de disquete (raw: 160 KiB a 2,88 MiB) no drive seguinte (A:, depois B:). */
    public static DiskImage floppy(String path, boolean readOnly) {
        return fromPath(path, readOnly, Type.FLOPPY);
    }

    public static DiskImage floppy(ParcelFileDescriptor pfd, boolean readOnly) {
        return fromFileDescriptor(pfd, readOnly, Type.FLOPPY);
    }

    /** Imagem ISO como CD/DVD-ROM ATAPI (somente leitura). */
    public static DiskImage cdrom(String isoPath) {
        return fromPath(isoPath, true, Type.CDROM);
    }

    public static DiskImage cdrom(ParcelFileDescriptor pfd) {
        return fromFileDescriptor(pfd, true, Type.CDROM);
    }

    public boolean isReadOnly() { return readOnly; }

    public Type getType() { return type; }
}
