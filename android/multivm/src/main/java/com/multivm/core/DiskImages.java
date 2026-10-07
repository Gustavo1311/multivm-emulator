package com.multivm.core;

import android.os.ParcelFileDescriptor;

import java.io.IOException;

/**
 * Utilitarios de imagens de disco: informacao, criacao e conversao.
 *
 * <p>Formatos lidos e gravados: raw, qcow2, VDI (VirtualBox), VMDK (VMware), VHD e VHDX
 * (Hyper-V/Virtual PC) e MVD, o formato proprio do MultiVM. O MVD foi feito para o
 * armazenamento do Android: o arquivo cresce conforme o uso (um disco de 64 GiB vazio
 * ocupa poucos MiB), metadados ficam na memoria e sao gravados em lote, o TRIM do
 * convidado devolve espaco para reuso e a conversao pode comprimir os blocos (LZ4).
 *
 * <p>As operacoes sao sincronas e podem demorar (conversao): chame fora da thread de UI.
 */
public final class DiskImages {

    private DiskImages() {}

    /** Formato de saida da conversao. */
    public enum Format {
        RAW(0), MVD(1);

        final int nativeId;

        Format(int id) { nativeId = id; }
    }

    /** Informacoes de uma imagem. */
    public static final class Info {
        /** "raw", "qcow2", "vdi", "vmdk", "vhd", "vhdx" ou "mvd". */
        public final String format;
        /** Tamanho do disco visto pelo convidado, em bytes. */
        public final long virtualSize;
        /** Bytes ocupados pelo arquivo. */
        public final long fileSize;
        /** O formato e o arquivo aceitam escrita. */
        public final boolean writable;

        Info(String format, long virtualSize, long fileSize, boolean writable) {
            this.format = format;
            this.virtualSize = virtualSize;
            this.fileSize = fileSize;
            this.writable = writable;
        }

        @Override
        public String toString() {
            return format + ", " + virtualSize + " bytes (arquivo: " + fileSize + ")";
        }
    }

    /** Progresso da conversao; devolva false para cancelar. */
    public interface Progress {
        boolean onProgress(long done, long total);
    }

    public static Info info(String path) throws IOException {
        if (path == null) throw new NullPointerException("path");
        return NativeBridge.nativeImageInfo(path, -1);
    }

    /** O descritor nao e fechado. */
    public static Info info(ParcelFileDescriptor pfd) throws IOException {
        if (pfd == null) throw new NullPointerException("pfd");
        return NativeBridge.nativeImageInfo(null, pfd.getFd());
    }

    /** Cria um disco MVD vazio (blocos de 256 KiB). O arquivo nao pode existir. */
    public static void createMvd(String path, long sizeBytes) throws IOException {
        createMvd(path, sizeBytes, 0);
    }

    /** blockSize: potencia de 2 entre 64 KiB e 4 MiB, ou 0 para o padrao. */
    public static void createMvd(String path, long sizeBytes, int blockSize) throws IOException {
        if (path == null) throw new NullPointerException("path");
        if (sizeBytes <= 0) throw new IllegalArgumentException("tamanho invalido");
        NativeBridge.nativeImageCreate(path, sizeBytes, blockSize);
    }

    /**
     * Converte uma imagem de qualquer formato suportado. O destino e gravado em
     * {@code dst + ".part"} e renomeado no fim; em erro ou cancelamento nada fica.
     *
     * @param compress so para MVD: comprime os blocos com LZ4 (bom para imagens base
     *                 que mudam pouco; blocos regravados voltam a ser descomprimidos)
     */
    public static void convert(String src, String dst, Format format, boolean compress, Progress progress)
            throws IOException {
        if (src == null) throw new NullPointerException("src");
        convert(src, -1, dst, format, compress, progress);
    }

    /** Origem por descritor (Storage Access Framework); o descritor nao e fechado. */
    public static void convert(ParcelFileDescriptor src, String dst, Format format, boolean compress,
                               Progress progress) throws IOException {
        if (src == null) throw new NullPointerException("src");
        convert(null, src.getFd(), dst, format, compress, progress);
    }

    private static void convert(String src, int fd, String dst, Format format, boolean compress, Progress progress)
            throws IOException {
        if (dst == null) throw new NullPointerException("dst");
        if (format == null) throw new NullPointerException("format");
        NativeBridge.nativeImageConvert(src, fd, dst, format.nativeId, compress ? 1 : 0, 0, progress);
    }
}
