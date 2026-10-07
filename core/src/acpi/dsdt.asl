/*
 * DSDT da maquina "pc" do MultiVM (i440FX + PIIX3, uma CPU).
 *
 * Compilado com tools/gen_dsdt.sh (iasl) para dsdt_aml.h, que fica no
 * repositorio: o build normal nao precisa do iasl.
 *
 * A janela de memoria PCI (PMEM) comeca em 0x80000000; acpi.c troca o inicio
 * para 0xC0000000 quando a RAM passa de 2 GiB (valores marcados abaixo).
 */
DefinitionBlock ("dsdt.aml", "DSDT", 2, "MULTVM", "MVMPC", 0x00000001)
{
    Scope (\_SB)
    {
        Processor (CPU0, 0x00, 0x00000000, 0x00) {}

        Device (HPET)
        {
            Name (_HID, EisaId ("PNP0103"))
            Name (_UID, Zero)
            Method (_STA, 0, NotSerialized) { Return (0x0F) }
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadOnly, 0xFED00000, 0x00000400)
            })
        }

        Device (PCI0)
        {
            Name (_HID, EisaId ("PNP0A03"))
            Name (_UID, Zero)
            Name (_BBN, Zero)

            Name (_CRS, ResourceTemplate ()
            {
                WordBusNumber (ResourceProducer, MinFixed, MaxFixed, PosDecode,
                    0x0000, 0x0000, 0x00FF, 0x0000, 0x0100)
                IO (Decode16, 0x0CF8, 0x0CF8, 0x01, 0x08)
                WordIO (ResourceProducer, MinFixed, MaxFixed, PosDecode, EntireRange,
                    0x0000, 0x0000, 0x0CF7, 0x0000, 0x0CF8)
                WordIO (ResourceProducer, MinFixed, MaxFixed, PosDecode, EntireRange,
                    0x0000, 0x0D00, 0xFFFF, 0x0000, 0xF300)
                DWordMemory (ResourceProducer, PosDecode, MinFixed, MaxFixed, Cacheable, ReadWrite,
                    0x00000000, 0x000A0000, 0x000BFFFF, 0x00000000, 0x00020000)
                /* janela PCI: inicio e tamanho sao ajustados por acpi.c */
                DWordMemory (ResourceProducer, PosDecode, MinFixed, MaxFixed, NonCacheable, ReadWrite,
                    0x00000000, 0x80000000, 0xFEBFFFFF, 0x00000000, 0x7EC00000)
            })

            /* INTx: pino p do slot s vai para o link (s + p) mod 4, como no PIIX3 */
            Name (_PRT, Package ()
            {
                Package () { 0x0001FFFF, 0, LNKB, 0 }, Package () { 0x0001FFFF, 1, LNKC, 0 },
                Package () { 0x0001FFFF, 2, LNKD, 0 }, Package () { 0x0001FFFF, 3, LNKA, 0 },
                Package () { 0x0002FFFF, 0, LNKC, 0 }, Package () { 0x0002FFFF, 1, LNKD, 0 },
                Package () { 0x0002FFFF, 2, LNKA, 0 }, Package () { 0x0002FFFF, 3, LNKB, 0 },
                Package () { 0x0003FFFF, 0, LNKD, 0 }, Package () { 0x0003FFFF, 1, LNKA, 0 },
                Package () { 0x0003FFFF, 2, LNKB, 0 }, Package () { 0x0003FFFF, 3, LNKC, 0 },
                Package () { 0x0004FFFF, 0, LNKA, 0 }, Package () { 0x0004FFFF, 1, LNKB, 0 },
                Package () { 0x0004FFFF, 2, LNKC, 0 }, Package () { 0x0004FFFF, 3, LNKD, 0 },
                Package () { 0x0005FFFF, 0, LNKB, 0 }, Package () { 0x0005FFFF, 1, LNKC, 0 },
                Package () { 0x0005FFFF, 2, LNKD, 0 }, Package () { 0x0005FFFF, 3, LNKA, 0 },
                Package () { 0x0006FFFF, 0, LNKC, 0 }, Package () { 0x0006FFFF, 1, LNKD, 0 },
                Package () { 0x0006FFFF, 2, LNKA, 0 }, Package () { 0x0006FFFF, 3, LNKB, 0 },
                Package () { 0x0007FFFF, 0, LNKD, 0 }, Package () { 0x0007FFFF, 1, LNKA, 0 },
                Package () { 0x0007FFFF, 2, LNKB, 0 }, Package () { 0x0007FFFF, 3, LNKC, 0 },
                Package () { 0x0008FFFF, 0, LNKA, 0 }, Package () { 0x0008FFFF, 1, LNKB, 0 },
                Package () { 0x0008FFFF, 2, LNKC, 0 }, Package () { 0x0008FFFF, 3, LNKD, 0 },
                Package () { 0x0009FFFF, 0, LNKB, 0 }, Package () { 0x0009FFFF, 1, LNKC, 0 },
                Package () { 0x0009FFFF, 2, LNKD, 0 }, Package () { 0x0009FFFF, 3, LNKA, 0 },
                Package () { 0x000AFFFF, 0, LNKC, 0 }, Package () { 0x000AFFFF, 1, LNKD, 0 },
                Package () { 0x000AFFFF, 2, LNKA, 0 }, Package () { 0x000AFFFF, 3, LNKB, 0 },
                Package () { 0x000BFFFF, 0, LNKD, 0 }, Package () { 0x000BFFFF, 1, LNKA, 0 },
                Package () { 0x000BFFFF, 2, LNKB, 0 }, Package () { 0x000BFFFF, 3, LNKC, 0 },
                Package () { 0x000CFFFF, 0, LNKA, 0 }, Package () { 0x000CFFFF, 1, LNKB, 0 },
                Package () { 0x000CFFFF, 2, LNKC, 0 }, Package () { 0x000CFFFF, 3, LNKD, 0 },
                Package () { 0x000DFFFF, 0, LNKB, 0 }, Package () { 0x000DFFFF, 1, LNKC, 0 },
                Package () { 0x000DFFFF, 2, LNKD, 0 }, Package () { 0x000DFFFF, 3, LNKA, 0 },
                Package () { 0x000EFFFF, 0, LNKC, 0 }, Package () { 0x000EFFFF, 1, LNKD, 0 },
                Package () { 0x000EFFFF, 2, LNKA, 0 }, Package () { 0x000EFFFF, 3, LNKB, 0 },
                Package () { 0x000FFFFF, 0, LNKD, 0 }, Package () { 0x000FFFFF, 1, LNKA, 0 },
                Package () { 0x000FFFFF, 2, LNKB, 0 }, Package () { 0x000FFFFF, 3, LNKC, 0 },
                Package () { 0x0010FFFF, 0, LNKA, 0 }, Package () { 0x0010FFFF, 1, LNKB, 0 },
                Package () { 0x0010FFFF, 2, LNKC, 0 }, Package () { 0x0010FFFF, 3, LNKD, 0 },
                Package () { 0x0011FFFF, 0, LNKB, 0 }, Package () { 0x0011FFFF, 1, LNKC, 0 },
                Package () { 0x0011FFFF, 2, LNKD, 0 }, Package () { 0x0011FFFF, 3, LNKA, 0 },
                Package () { 0x0012FFFF, 0, LNKC, 0 }, Package () { 0x0012FFFF, 1, LNKD, 0 },
                Package () { 0x0012FFFF, 2, LNKA, 0 }, Package () { 0x0012FFFF, 3, LNKB, 0 },
                Package () { 0x0013FFFF, 0, LNKD, 0 }, Package () { 0x0013FFFF, 1, LNKA, 0 },
                Package () { 0x0013FFFF, 2, LNKB, 0 }, Package () { 0x0013FFFF, 3, LNKC, 0 },
                Package () { 0x0014FFFF, 0, LNKA, 0 }, Package () { 0x0014FFFF, 1, LNKB, 0 },
                Package () { 0x0014FFFF, 2, LNKC, 0 }, Package () { 0x0014FFFF, 3, LNKD, 0 },
                Package () { 0x0015FFFF, 0, LNKB, 0 }, Package () { 0x0015FFFF, 1, LNKC, 0 },
                Package () { 0x0015FFFF, 2, LNKD, 0 }, Package () { 0x0015FFFF, 3, LNKA, 0 },
                Package () { 0x0016FFFF, 0, LNKC, 0 }, Package () { 0x0016FFFF, 1, LNKD, 0 },
                Package () { 0x0016FFFF, 2, LNKA, 0 }, Package () { 0x0016FFFF, 3, LNKB, 0 },
                Package () { 0x0017FFFF, 0, LNKD, 0 }, Package () { 0x0017FFFF, 1, LNKA, 0 },
                Package () { 0x0017FFFF, 2, LNKB, 0 }, Package () { 0x0017FFFF, 3, LNKC, 0 },
                Package () { 0x0018FFFF, 0, LNKA, 0 }, Package () { 0x0018FFFF, 1, LNKB, 0 },
                Package () { 0x0018FFFF, 2, LNKC, 0 }, Package () { 0x0018FFFF, 3, LNKD, 0 },
                Package () { 0x0019FFFF, 0, LNKB, 0 }, Package () { 0x0019FFFF, 1, LNKC, 0 },
                Package () { 0x0019FFFF, 2, LNKD, 0 }, Package () { 0x0019FFFF, 3, LNKA, 0 },
                Package () { 0x001AFFFF, 0, LNKC, 0 }, Package () { 0x001AFFFF, 1, LNKD, 0 },
                Package () { 0x001AFFFF, 2, LNKA, 0 }, Package () { 0x001AFFFF, 3, LNKB, 0 },
                Package () { 0x001BFFFF, 0, LNKD, 0 }, Package () { 0x001BFFFF, 1, LNKA, 0 },
                Package () { 0x001BFFFF, 2, LNKB, 0 }, Package () { 0x001BFFFF, 3, LNKC, 0 },
                Package () { 0x001CFFFF, 0, LNKA, 0 }, Package () { 0x001CFFFF, 1, LNKB, 0 },
                Package () { 0x001CFFFF, 2, LNKC, 0 }, Package () { 0x001CFFFF, 3, LNKD, 0 },
                Package () { 0x001DFFFF, 0, LNKB, 0 }, Package () { 0x001DFFFF, 1, LNKC, 0 },
                Package () { 0x001DFFFF, 2, LNKD, 0 }, Package () { 0x001DFFFF, 3, LNKA, 0 },
                Package () { 0x001EFFFF, 0, LNKC, 0 }, Package () { 0x001EFFFF, 1, LNKD, 0 },
                Package () { 0x001EFFFF, 2, LNKA, 0 }, Package () { 0x001EFFFF, 3, LNKB, 0 },
                Package () { 0x001FFFFF, 0, LNKD, 0 }, Package () { 0x001FFFFF, 1, LNKA, 0 },
                Package () { 0x001FFFFF, 2, LNKB, 0 }, Package () { 0x001FFFFF, 3, LNKC, 0 },
            })

            /* PIIX3 (00:01.0): ponte PCI-ISA com os registradores PIRQ 0x60-0x63 */
            Device (ISA)
            {
                Name (_ADR, 0x00010000)

                OperationRegion (PIRQ, PCI_Config, 0x60, 0x04)
                Field (PIRQ, ByteAcc, NoLock, Preserve)
                {
                    PRQA, 8,
                    PRQB, 8,
                    PRQC, 8,
                    PRQD, 8
                }

                Device (PIC)
                {
                    Name (_HID, EisaId ("PNP0000"))
                    Name (_CRS, ResourceTemplate ()
                    {
                        IO (Decode16, 0x0020, 0x0020, 0x01, 0x02)
                        IO (Decode16, 0x00A0, 0x00A0, 0x01, 0x02)
                        IO (Decode16, 0x04D0, 0x04D0, 0x01, 0x02)
                        IRQNoFlags () {2}
                    })
                }

                Device (DMAC)
                {
                    Name (_HID, EisaId ("PNP0200"))
                    Name (_CRS, ResourceTemplate ()
                    {
                        IO (Decode16, 0x0000, 0x0000, 0x01, 0x10)
                        IO (Decode16, 0x0081, 0x0081, 0x01, 0x0F)
                        IO (Decode16, 0x00C0, 0x00C0, 0x01, 0x20)
                        DMA (Compatibility, NotBusMaster, Transfer8_16) {4}
                    })
                }

                Device (TMR)
                {
                    Name (_HID, EisaId ("PNP0100"))
                    Name (_CRS, ResourceTemplate ()
                    {
                        IO (Decode16, 0x0040, 0x0040, 0x01, 0x04)
                        IRQNoFlags () {0}
                    })
                }

                Device (RTC)
                {
                    Name (_HID, EisaId ("PNP0B00"))
                    Name (_CRS, ResourceTemplate ()
                    {
                        IO (Decode16, 0x0070, 0x0070, 0x01, 0x02)
                        IRQNoFlags () {8}
                    })
                }

                Device (SPKR)
                {
                    Name (_HID, EisaId ("PNP0800"))
                    Name (_CRS, ResourceTemplate ()
                    {
                        IO (Decode16, 0x0061, 0x0061, 0x01, 0x01)
                    })
                }

                Device (KBD)
                {
                    Name (_HID, EisaId ("PNP0303"))
                    Method (_STA, 0, NotSerialized) { Return (0x0F) }
                    Name (_CRS, ResourceTemplate ()
                    {
                        IO (Decode16, 0x0060, 0x0060, 0x01, 0x01)
                        IO (Decode16, 0x0064, 0x0064, 0x01, 0x01)
                        IRQNoFlags () {1}
                    })
                }

                Device (MOU)
                {
                    Name (_HID, EisaId ("PNP0F13"))
                    Method (_STA, 0, NotSerialized) { Return (0x0F) }
                    Name (_CRS, ResourceTemplate ()
                    {
                        IRQNoFlags () {12}
                    })
                }

                Device (COM1)
                {
                    Name (_HID, EisaId ("PNP0501"))
                    Name (_UID, One)
                    Method (_STA, 0, NotSerialized) { Return (0x0F) }
                    Name (_CRS, ResourceTemplate ()
                    {
                        IO (Decode16, 0x03F8, 0x03F8, 0x00, 0x08)
                        IRQNoFlags () {4}
                    })
                }

                /* controlador de disquete: so aparece quando ha drives (FDEN trocado pelo acpi.c) */
                Device (FDC0)
                {
                    Name (_HID, EisaId ("PNP0700"))
                    Name (FDEN, 0x4644434E) /* bit 0 = A:, bit 1 = B: */
                    Method (_STA, 0, NotSerialized)
                    {
                        If (LEqual (FDEN, Zero)) { Return (Zero) }
                        Return (0x0F)
                    }
                    Name (_CRS, ResourceTemplate ()
                    {
                        IO (Decode16, 0x03F2, 0x03F2, 0x00, 0x04)
                        IO (Decode16, 0x03F7, 0x03F7, 0x00, 0x01)
                        IRQNoFlags () {6}
                        DMA (Compatibility, NotBusMaster, Transfer8) {2}
                    })
                    /* drives presentes: 4 DWORDs (A: a D:) + fita */
                    Method (_FDE, 0, NotSerialized)
                    {
                        Store (Buffer (0x14) {}, Local0)
                        CreateDWordField (Local0, 0x00, DRVA)
                        CreateDWordField (Local0, 0x04, DRVB)
                        And (FDEN, One, DRVA)
                        ShiftRight (And (FDEN, 0x02), One, DRVB)
                        Return (Local0)
                    }
                }

                /* recursos da placa-mae que nao pertencem a outro dispositivo */
                Device (MBRS)
                {
                    Name (_HID, EisaId ("PNP0C02"))
                    Name (_UID, One)
                    Name (_CRS, ResourceTemplate ()
                    {
                        IO (Decode16, 0x0092, 0x0092, 0x01, 0x01)   /* A20 rapido */
                        IO (Decode16, 0x0510, 0x0510, 0x01, 0x0C)   /* fw_cfg */
                        IO (Decode16, 0x0600, 0x0600, 0x01, 0x40)   /* PM ACPI */
                        IO (Decode16, 0xAFE0, 0xAFE0, 0x01, 0x04)   /* GPE0 */
                        IO (Decode16, 0x0CF9, 0x0CF9, 0x01, 0x01)   /* reset */
                        Memory32Fixed (ReadOnly, 0xFEC00000, 0x00001000)  /* IOAPIC */
                        Memory32Fixed (ReadOnly, 0xFEE00000, 0x00001000)  /* APIC local */
                    })
                }
            }

            /* status/_CRS comuns dos links PIRQ (bit 7 = desabilitado) */
            Method (IQST, 1, NotSerialized)
            {
                If ((Arg0 & 0x80)) { Return (0x09) }
                Return (0x0B)
            }

            Method (IQCR, 1, Serialized)
            {
                Name (PRR0, ResourceTemplate ()
                {
                    Interrupt (ResourceConsumer, Level, ActiveHigh, Shared, , , ) { 0 }
                })
                CreateDWordField (PRR0, 0x05, PRRI)
                If ((Arg0 < 0x80)) { PRRI = (Arg0 & 0x0F) }
                Return (PRR0)
            }
        }

        Device (LNKA)
        {
            Name (_HID, EisaId ("PNP0C0F"))
            Name (_UID, One)
            Name (_PRS, ResourceTemplate ()
            {
                Interrupt (ResourceConsumer, Level, ActiveHigh, Shared, , , ) { 5, 10, 11 }
            })
            Method (_STA, 0, NotSerialized) { Return (\_SB.PCI0.IQST (\_SB.PCI0.ISA.PRQA)) }
            Method (_DIS, 0, NotSerialized) { \_SB.PCI0.ISA.PRQA |= 0x80 }
            Method (_CRS, 0, NotSerialized) { Return (\_SB.PCI0.IQCR (\_SB.PCI0.ISA.PRQA)) }
            Method (_SRS, 1, NotSerialized)
            {
                CreateDWordField (Arg0, 0x05, PRRI)
                \_SB.PCI0.ISA.PRQA = PRRI
            }
        }

        Device (LNKB)
        {
            Name (_HID, EisaId ("PNP0C0F"))
            Name (_UID, 0x02)
            Name (_PRS, ResourceTemplate ()
            {
                Interrupt (ResourceConsumer, Level, ActiveHigh, Shared, , , ) { 5, 10, 11 }
            })
            Method (_STA, 0, NotSerialized) { Return (\_SB.PCI0.IQST (\_SB.PCI0.ISA.PRQB)) }
            Method (_DIS, 0, NotSerialized) { \_SB.PCI0.ISA.PRQB |= 0x80 }
            Method (_CRS, 0, NotSerialized) { Return (\_SB.PCI0.IQCR (\_SB.PCI0.ISA.PRQB)) }
            Method (_SRS, 1, NotSerialized)
            {
                CreateDWordField (Arg0, 0x05, PRRI)
                \_SB.PCI0.ISA.PRQB = PRRI
            }
        }

        Device (LNKC)
        {
            Name (_HID, EisaId ("PNP0C0F"))
            Name (_UID, 0x03)
            Name (_PRS, ResourceTemplate ()
            {
                Interrupt (ResourceConsumer, Level, ActiveHigh, Shared, , , ) { 5, 10, 11 }
            })
            Method (_STA, 0, NotSerialized) { Return (\_SB.PCI0.IQST (\_SB.PCI0.ISA.PRQC)) }
            Method (_DIS, 0, NotSerialized) { \_SB.PCI0.ISA.PRQC |= 0x80 }
            Method (_CRS, 0, NotSerialized) { Return (\_SB.PCI0.IQCR (\_SB.PCI0.ISA.PRQC)) }
            Method (_SRS, 1, NotSerialized)
            {
                CreateDWordField (Arg0, 0x05, PRRI)
                \_SB.PCI0.ISA.PRQC = PRRI
            }
        }

        Device (LNKD)
        {
            Name (_HID, EisaId ("PNP0C0F"))
            Name (_UID, 0x04)
            Name (_PRS, ResourceTemplate ()
            {
                Interrupt (ResourceConsumer, Level, ActiveHigh, Shared, , , ) { 5, 10, 11 }
            })
            Method (_STA, 0, NotSerialized) { Return (\_SB.PCI0.IQST (\_SB.PCI0.ISA.PRQD)) }
            Method (_DIS, 0, NotSerialized) { \_SB.PCI0.ISA.PRQD |= 0x80 }
            Method (_CRS, 0, NotSerialized) { Return (\_SB.PCI0.IQCR (\_SB.PCI0.ISA.PRQD)) }
            Method (_SRS, 1, NotSerialized)
            {
                CreateDWordField (Arg0, 0x05, PRRI)
                \_SB.PCI0.ISA.PRQD = PRRI
            }
        }
    }

    /* modo de interrupcao escolhido pelo SO (0 = 8259, 1 = APIC) */
    Name (PICF, Zero)
    Method (\_PIC, 1, NotSerialized) { PICF = Arg0 }

    /* S5 (desligado): SLP_TYP = 0 no PM1_CNT */
    Name (\_S5, Package (0x04) { Zero, Zero, Zero, Zero })
}
