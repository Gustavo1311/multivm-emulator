package com.multivm.app;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.AlertDialog;
import android.content.Intent;
import android.graphics.Typeface;
import android.os.Bundle;
import android.text.SpannableStringBuilder;
import android.text.Spanned;
import android.text.style.ForegroundColorSpan;
import android.view.Gravity;
import android.view.View;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.RadioButton;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.ViewFlipper;

import com.multivm.core.Architecture;

import java.io.File;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

/**
 * Boas-vindas e assistente da primeira VM. As opcoes ficam divididas em categorias
 * (uma pagina cada) e o resumo aparece antes de criar. Grava a configuracao com
 * {@link VmSettings}, a mesma da MainActivity.
 */
public class WizardActivity extends BaseActivity implements MediaListView.Host {

    /** extra do Intent: aberto pela tela inicial (nao e a primeira execucao: comeca em Sistema) */
    static final String EXTRA_FROM_HOME = "fromHome";

    private static final int STEP_WELCOME = 0, STEP_SYSTEM = 1, STEP_HARDWARE = 2, STEP_NET = 3, STEP_STORAGE = 4,
            STEP_BOOT = 5, STEP_SUMMARY = 6;
    private static final String[] STEP_NAMES = {"Boas-vindas", "Sistema", "Hardware", "Som e rede", "Armazenamento",
            "Inicialização", "Resumo"};

    /* requestCodes: kernel, initrd e as listas de midias (REQ_MEDIA + tipo) */
    private static final int REQ_KERNEL = 0, REQ_INITRD = 1, REQ_MEDIA = 10;
    private static final long DEFAULT_DISK_GB = 8;

    private String kernel, initrd;
    private int step;
    private boolean fromHome, creating;
    /** primeira etapa: boas-vindas so na primeira execucao */
    private int firstStep;
    /** ultimo nome de disco sugerido a partir do nome da VM */
    private String autoDiskName = "";

    private ViewFlipper pages;
    private ScrollView scroll;
    private View header;
    private LinearLayout stepBar, summary;
    private TextView stepLabel, modeHelp, ramHelp, bootHelp, kernelName, initrdName;
    private EditText name, ram, cmdline, fbSize;
    private Spinner arch, bootOrder;
    private SeekBar ramBar;
    private RadioButton modeBios, modeKernel, ctrlSata, ctrlVirtio;
    private CheckBox fb, startNow;
    private View ctrlBox, biosBox, kernelBox;
    private MediaListView disks, cds, floppies;
    private Button back, next;
    private long deviceRam;
    private Spinner os;
    private TextView osHelp;
    /* som e rede */
    private Spinner sound, nic;
    private CheckBox mic, vnc, vncLan;
    private EditText vncPort, vncPassword;
    private View vncGroup;
    private final List<String> forwards = new ArrayList<>();
    private ForwardList fwd;
    private final DiskCreator diskCreator = new DiskCreator(this);
    /** sistema cujas placas sugeridas ja foram aplicadas (-1 = nenhum) */
    private int appliedOs = -1;

    /* sistema convidado -> placas de som e de rede com driver nativo (indices de VmSettings) */
    private static final String[] OS_NAMES = {"Linux", "Windows XP", "Windows 7, 10, 11 ou Server", "ReactOS",
            "Outro (DOS, BSD...)"};
    private static final int[] OS_SOUND = {2, 1, 2, 1, 0};
    private static final int[] OS_NIC = {3, 1, 2, 1, 0};

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        setContentView(R.layout.activity_wizard);
        fromHome = getIntent().getBooleanExtra(EXTRA_FROM_HOME, false);
        firstStep = fromHome ? STEP_SYSTEM : STEP_WELCOME;
        step = firstStep;

        pages = findViewById(R.id.pages);
        scroll = findViewById(R.id.scroll);
        header = findViewById(R.id.header);
        stepBar = findViewById(R.id.stepBar);
        stepLabel = findViewById(R.id.stepLabel);
        summary = findViewById(R.id.wSummary);
        modeHelp = findViewById(R.id.wModeHelp);
        ramHelp = findViewById(R.id.wRamHelp);
        bootHelp = findViewById(R.id.wBootHelp);
        kernelName = findViewById(R.id.wKernelName);
        initrdName = findViewById(R.id.wInitrdName);
        name = findViewById(R.id.wName);
        ram = findViewById(R.id.wRam);
        cmdline = findViewById(R.id.wCmdline);
        fbSize = findViewById(R.id.wFbSize);
        fbSize.setText(AppSettings.load(this).defaultResolution);
        arch = findViewById(R.id.wArch);
        bootOrder = findViewById(R.id.wBootOrder);
        ramBar = findViewById(R.id.wRamBar);
        modeBios = findViewById(R.id.wModeBios);
        modeKernel = findViewById(R.id.wModeKernel);
        ctrlSata = findViewById(R.id.wCtrlSata);
        ctrlVirtio = findViewById(R.id.wCtrlVirtio);
        fb = findViewById(R.id.wFb);
        startNow = findViewById(R.id.wStartNow);
        ctrlBox = findViewById(R.id.wCtrlBox);
        biosBox = findViewById(R.id.wBiosBox);
        kernelBox = findViewById(R.id.wKernelBox);
        disks = findViewById(R.id.wDisks);
        cds = findViewById(R.id.wCds);
        floppies = findViewById(R.id.wFloppies);
        back = findViewById(R.id.wBack);
        next = findViewById(R.id.wNext);

        pages.setInAnimation(this, R.anim.wiz_in);
        pages.setOutAnimation(this, R.anim.wiz_out);

        arch.setAdapter(spinnerAdapter(AppFiles.ARCH_NAMES));
        arch.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> p, View v, int pos, long id) { updateMode(); }

            @Override
            public void onNothingSelected(AdapterView<?> p) {}
        });
        modeBios.setOnCheckedChangeListener((b, c) -> updateMode());
        modeKernel.setOnCheckedChangeListener((b, c) -> updateMode());
        bootOrder.setAdapter(spinnerAdapter(VmSettings.BOOT_NAMES));
        os = findViewById(R.id.wOs);
        osHelp = findViewById(R.id.wOsHelp);
        os.setAdapter(spinnerAdapter(OS_NAMES));
        os.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> p, View v, int pos, long id) {
                osHelp.setText("Placas sugeridas — som: " + VmSettings.SOUND_NAMES[OS_SOUND[pos]] + "; rede: "
                        + VmSettings.NIC_NAMES[OS_NIC[pos]] + ". Você pode trocar na próxima etapa, “Som e rede”.");
                updateRamHelp();
            }

            @Override
            public void onNothingSelected(AdapterView<?> p) {}
        });
        sound = findViewById(R.id.wSound);
        nic = findViewById(R.id.wNic);
        mic = findViewById(R.id.wMic);
        vnc = findViewById(R.id.wVnc);
        vncLan = findViewById(R.id.wVncLan);
        vncPort = findViewById(R.id.wVncPort);
        vncPassword = findViewById(R.id.wVncPassword);
        vncGroup = findViewById(R.id.wVncGroup);
        sound.setAdapter(spinnerAdapter(VmSettings.SOUND_NAMES));
        nic.setAdapter(spinnerAdapter(VmSettings.NIC_NAMES));
        sound.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> p, View v, int pos, long id) {
                mic.setVisibility(pos == VmSettings.SOUND_NONE ? View.GONE : View.VISIBLE);
            }

            @Override
            public void onNothingSelected(AdapterView<?> p) {}
        });
        nic.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> p, View v, int pos, long id) {
                fwd.render(pos == VmSettings.NIC_NONE);
            }

            @Override
            public void onNothingSelected(AdapterView<?> p) {}
        });
        vnc.setOnCheckedChangeListener((b, c) -> vncGroup.setVisibility(c ? View.VISIBLE : View.GONE));
        fwd = new ForwardList(this, findViewById(R.id.wFwdList), forwards, null);
        findViewById(R.id.wFwdAdd).setOnClickListener(v -> fwd.addDialog());
        deviceRam = deviceRamMb();
        AppFiles.bindRam(ram, ramBar, this::updateRamHelp);
        fb.setOnCheckedChangeListener((b, c) -> fbSize.setVisibility(c ? View.VISIBLE : View.GONE));

        disks.setup(VmSettings.KIND_DISK, this);
        disks.setShowConvert(false);
        cds.setup(VmSettings.KIND_CD, this);
        floppies.setup(VmSettings.KIND_FLOPPY, this);

        findViewById(R.id.wKernelPick).setOnClickListener(v -> pick(REQ_KERNEL, false));
        findViewById(R.id.wInitrdPick).setOnClickListener(v -> pick(REQ_INITRD, false));
        findViewById(R.id.wKernelClear).setOnClickListener(v -> setKernel(null));
        findViewById(R.id.wInitrdClear).setOnClickListener(v -> setInitrd(null));

        back.setOnClickListener(v -> onBackClicked());
        next.setOnClickListener(v -> onNextClicked());

        if (saved != null) {
            step = saved.getInt("step", firstStep);
            autoDiskName = saved.getString("autoDiskName", "");
            kernel = saved.getString("kernel");
            appliedOs = saved.getInt("appliedOs", -1);
            String[] fw = saved.getStringArray("forwards");
            if (fw != null) java.util.Collections.addAll(forwards, fw);
            initrd = saved.getString("initrd");
            MediaListView[] lists = {disks, cds, floppies};
            for (int k = 0; k < lists.length; k++) {
                List<VmSettings.Media> items = new ArrayList<>();
                String[] enc = saved.getStringArray("media" + k);
                if (enc != null) for (String e : enc) items.add(VmSettings.Media.decode(e));
                lists[k].setItems(items);
            }
        } else {
            /* nome padrao: "Minha VM", "Minha VM 2", ... */
            int n = VmSettings.ids(this).size();
            ram.setText(String.valueOf(AppFiles.defaultRamMb(this)));
            if (n > 0) name.setText("Minha VM " + (n + 1));
            /* sugestao inicial: um disco novo de 8 GiB (o nome acompanha o nome da VM) */
            suggestDiskName();
        }
        setKernel(kernel);
        setInitrd(initrd);
        updateMode();
        updateRamHelp();
        fbSize.setVisibility(fb.isChecked() ? View.VISIBLE : View.GONE);
        vncGroup.setVisibility(vnc.isChecked() ? View.VISIBLE : View.GONE);
        fwd.render(false);
    }

    @Override
    protected void onPostCreate(Bundle saved) {
        super.onPostCreate(saved);
        /* depois de onRestoreInstanceState: os campos ja tem os valores restaurados */
        updateMode();
        showStep(step, false);
    }

    @Override
    protected void onSaveInstanceState(Bundle out) {
        super.onSaveInstanceState(out);
        out.putInt("step", step);
        out.putString("autoDiskName", autoDiskName);
        out.putString("kernel", kernel);
        out.putInt("appliedOs", appliedOs);
        out.putStringArray("forwards", forwards.toArray(new String[0]));
        out.putString("initrd", initrd);
        MediaListView[] lists = {disks, cds, floppies};
        for (int k = 0; k < lists.length; k++) {
            List<VmSettings.Media> items = lists[k].items();
            String[] enc = new String[items.size()];
            for (int i = 0; i < enc.length; i++) enc[i] = items.get(i).encode();
            out.putStringArray("media" + k, enc);
        }
    }

    @Override
    public void onBackPressed() {
        if (creating) return;
        if (step > firstStep) showStep(step - 1, true);
        else if (fromHome) finish();
        else super.onBackPressed();
    }

    /* ---- navegacao ---- */

    private void onBackClicked() {
        if (step > firstStep) {
            showStep(step - 1, true);
            return;
        }
        /* primeira etapa: "Cancelar" (aberto pela tela inicial) ou "Pular" (primeira execucao) */
        if (fromHome) {
            finish();
            return;
        }
        VmSettings.markSetupDone(this);
        leave(null);
    }

    private void onNextClicked() {
        String err = validate(step);
        if (err != null) {
            new AlertDialog.Builder(this).setTitle(STEP_NAMES[step]).setMessage(err)
                    .setPositiveButton("OK", null).show();
            return;
        }
        if (step == STEP_SUMMARY) create();
        else showStep(step + 1, true);
    }

    private void showStep(int s, boolean animate) {
        if (step == STEP_SYSTEM && s > step) suggestDiskName();
        step = s;
        if (!animate) {
            pages.setInAnimation(null);
            pages.setOutAnimation(null);
        }
        pages.setDisplayedChild(s);
        if (!animate) {
            pages.setInAnimation(this, R.anim.wiz_in);
            pages.setOutAnimation(this, R.anim.wiz_out);
        }
        scroll.scrollTo(0, 0);
        if (s == STEP_BOOT) updateBootHelp();
        if (s == STEP_NET) applyOsDefaults();
        if (s == STEP_SUMMARY) buildSummary();

        header.setVisibility(s == STEP_WELCOME ? View.GONE : View.VISIBLE);
        int n = STEP_NAMES.length - 1;
        stepLabel.setText(String.format(Locale.ROOT, "ETAPA %d DE %d  ·  %s", s, n,
                STEP_NAMES[s].toUpperCase(Locale.ROOT)));
        stepBar.removeAllViews();
        int gap = dp(4);
        for (int i = 1; i <= n; i++) {
            View seg = new View(this);
            seg.setBackgroundColor(i <= s ? getColor(R.color.accent) : getColor(R.color.outline));
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.MATCH_PARENT, 1);
            if (i > 1) lp.setMarginStart(gap);
            stepBar.addView(seg, lp);
        }

        back.setText(s > firstStep ? "Voltar" : fromHome ? "Cancelar" : "Pular");
        next.setText(s == STEP_WELCOME ? "Começar" : s == STEP_SUMMARY ? "Criar VM" : "Avançar");
        next.setCompoundDrawablesRelativeWithIntrinsicBounds(s == STEP_SUMMARY ? R.drawable.ic_play : 0, 0, 0, 0);
    }

    /** Placas sugeridas para o sistema escolhido em Hardware (so quando o sistema muda). */
    private void applyOsDefaults() {
        int o = Math.max(0, os.getSelectedItemPosition());
        if (o == appliedOs)
            return;
        appliedOs = o;
        sound.setSelection(OS_SOUND[o]);
        nic.setSelection(OS_NIC[o]);
        ((TextView) findViewById(R.id.wNetIntro)).setText("Sugestão para " + OS_NAMES[o]
                + ": placas com driver já incluído no sistema. Troque se preferir.");
    }

    private int vncValue() {
        try {
            return Integer.parseInt(vncPort.getText().toString().trim());
        } catch (NumberFormatException e) {
            return -1;
        }
    }

    /** Mensagem do que falta na etapa, ou null se ela esta completa. */
    private String validate(int s) {
        switch (s) {
            case STEP_SYSTEM:
                if (vmName().isEmpty()) return "Dê um nome para a VM.";
                return null;
            case STEP_HARDWARE: {
                int mb = ramMb();
                if (mb < AppFiles.RAM_MIN || mb > AppFiles.RAM_MAX)
                    return "A memória deve ficar entre " + AppFiles.RAM_MIN + " e " + AppFiles.RAM_MAX + " MiB.";
                return null;
            }
            case STEP_NET:
                if (vnc.isChecked() && VmSettings.vncTcpPort(vncValue()) < 0)
                    return "VNC: use um número de tela de 0 a 99 (1 = porta 5901, como nos clientes VNC) ou uma porta "
                            + "de 1024 a 65535. O Android não deixa apps usarem portas abaixo de 1024.";
                return null;
            case STEP_STORAGE: {
                Set<String> names = new HashSet<>();
                File dir = AppFiles.diskDir(this);
                for (VmSettings.Media m : disks.items()) {
                    if (!m.isPending()) continue;
                    if (!names.add(m.newName))
                        return "Dois discos novos com o mesmo nome (" + m.newName + "). Toque num deles para renomear.";
                    if (new File(dir, m.newName).exists())
                        return "Já existe um disco chamado " + m.newName + ". Toque nele para escolher outro nome, "
                                + "ou remova-o e use “Adicionar imagem”.";
                }
                return null;
            }
            case STEP_BOOT:
                if (bios()) {
                    boolean boot = !cds.items().isEmpty() || !floppies.items().isEmpty();
                    if (!boot && disks.items().isEmpty())
                        return "A VM precisa de algo para dar boot: adicione uma ISO ou um disquete, ou volte e adicione um disco.";
                    if (!boot && allDisksNew())
                        return "Discos novos começam vazios: adicione a ISO (ou o disquete) do sistema que será instalado.";
                } else {
                    if (kernel == null) return "Escolha o arquivo do kernel.";
                    if (fb.isChecked() && parseFb() == null) return "Resolução inválida: use LARGURAxALTURA, por exemplo 800x600.";
                }
                return null;
            default:
                return null;
        }
    }

    /* ---- estado dos campos ---- */

    private Architecture selectedArch() {
        return AppFiles.ARCHS[Math.max(0, arch.getSelectedItemPosition())];
    }

    private boolean bios() {
        return AppFiles.isX86(selectedArch()) && modeBios.isChecked();
    }

    private boolean allDisksNew() {
        for (VmSettings.Media m : disks.items()) if (!m.isPending()) return false;
        return true;
    }

    private String vmName() {
        return name.getText().toString().trim();
    }

    private int ramMb() {
        try {
            return Integer.parseInt(ram.getText().toString().trim());
        } catch (NumberFormatException e) {
            return -1;
        }
    }

    private int[] parseFb() {
        String[] wh = fbSize.getText().toString().trim().toLowerCase(Locale.ROOT).split("x");
        try {
            int w = Integer.parseInt(wh[0].trim()), h = Integer.parseInt(wh[1].trim());
            return w > 0 && h > 0 ? new int[]{w, h} : null;
        } catch (Exception e) {
            return null;
        }
    }

    private void updateMode() {
        boolean x86 = AppFiles.isX86(selectedArch());
        modeBios.setEnabled(x86);
        if (!x86 && modeBios.isChecked()) modeKernel.setChecked(true);
        boolean b = bios();
        modeHelp.setText(x86
                ? b ? "A VM liga como um PC comum: a BIOS (SeaBIOS) dá boot pelo CD-ROM, pelo disco ou pelo disquete. Use para instalar Linux, Windows, ReactOS ou DOS."
                    : "O kernel Linux é carregado direto na memória, sem BIOS. A saída aparece no terminal serial."
                : "Em ARM a VM é uma placa “virt”: o kernel Linux é carregado direto, sem BIOS. Discos usam virtio.");
        ctrlBox.setVisibility(b ? View.VISIBLE : View.GONE);
        biosBox.setVisibility(b ? View.VISIBLE : View.GONE);
        kernelBox.setVisibility(b ? View.GONE : View.VISIBLE);
        cmdline.setHint("console=" + selectedArch().linuxConsole());
    }

    private void updateRamHelp() {
        int mb = ramMb();
        StringBuilder sb = new StringBuilder();
        if (deviceRam > 0) sb.append("Seu aparelho tem ").append(deviceRam).append(" MiB de RAM.");
        if (mb > 0 && deviceRam > 0 && mb > deviceRam / 2)
            sb.append("\nAtenção: isso é mais da metade da memória do aparelho; o Android pode fechar o app.");
        /* Linux e Windows 7+ (posicoes 0 e 2); XP, ReactOS e outros rodam com menos */
        int o = os == null ? 0 : Math.max(0, os.getSelectedItemPosition());
        String low = o == 0 || o == 2 ? AppFiles.lowRamWarning(mb) : null;
        if (low != null) sb.append(sb.length() > 0 ? "\n" : "").append(low);
        ramHelp.setText(sb);
    }

    private void updateBootHelp() {
        if (bios()) {
            bootHelp.setText(allDisksNew() && !disks.items().isEmpty()
                    ? "Adicione a ISO (ou o disquete) do sistema que será instalado no disco novo, por exemplo Alpine Linux, "
                    + "ReactOS, Windows ou FreeDOS. Depois da instalação, mude a ordem para “Disco rígido primeiro”."
                    : "Adicione ISOs e disquetes para dar boot ou para usar no sistema. Sem eles, a VM inicia pelo disco.");
        } else {
            bootHelp.setText("Escolha o kernel Linux (Image, zImage ou bzImage) e, se houver, o initrd.");
        }
    }

    /** Nome de arquivo livre a partir do nome da VM (ex.: "minha-vm.mvd", "minha-vm-2.mvd"). */
    private String freeDiskName(String base) {
        File dir = AppFiles.diskDir(this);
        Set<String> used = new HashSet<>();
        for (VmSettings.Media m : disks.items()) if (m.isPending()) used.add(m.newName);
        String n = base + ".mvd";
        for (int i = 2; new File(dir, n).exists() || used.contains(n); i++) n = base + "-" + i + ".mvd";
        return n;
    }

    private String nameBase() {
        String base = vmName().toLowerCase(Locale.ROOT).replaceAll("[^a-z0-9._-]+", "-").replaceAll("^-+|-+$", "");
        return base.isEmpty() ? "disco" : base;
    }

    /** O disco novo sugerido acompanha o nome da VM enquanto o usuario nao o renomear. */
    private void suggestDiskName() {
        List<VmSettings.Media> items = disks.items();
        if (items.isEmpty() && autoDiskName.isEmpty()) {
            autoDiskName = freeDiskName(nameBase());
            disks.addPending(autoDiskName, DEFAULT_DISK_GB);
            return;
        }
        for (VmSettings.Media m : items) {
            if (m.isPending() && m.newName.equals(autoDiskName)) {
                m.newName = "";
                autoDiskName = freeDiskName(nameBase());
                m.newName = autoDiskName;
                disks.render();
                return;
            }
        }
    }

    /* ---- arquivos ---- */

    private void pick(int req, boolean writable) {
        try {
            startActivityForResult(AppFiles.pickIntent(writable), req);
        } catch (Exception ex) {
            new AlertDialog.Builder(this).setMessage("Não há seletor de arquivos: " + ex.getMessage())
                    .setPositiveButton("OK", null).show();
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (diskCreator.onActivityResult(requestCode, resultCode, data)) return;
        if (resultCode != RESULT_OK || data == null || data.getData() == null) return;
        String uri = AppFiles.takePersistable(this, data);
        if (requestCode == REQ_KERNEL) setKernel(uri);
        else if (requestCode == REQ_INITRD) setInitrd(uri);
        else if (requestCode == REQ_MEDIA + VmSettings.KIND_DISK) disks.add(uri);
        else if (requestCode == REQ_MEDIA + VmSettings.KIND_CD) cds.add(uri);
        else if (requestCode == REQ_MEDIA + VmSettings.KIND_FLOPPY) floppies.add(uri);
    }

    private void setKernel(String v) {
        kernel = v;
        kernelName.setText(v == null ? "(nenhum)" : AppFiles.displayName(this, v));
    }

    private void setInitrd(String v) {
        initrd = v;
        initrdName.setText(v == null ? "(nenhum)" : AppFiles.displayName(this, v));
    }

    /* ---- MediaListView.Host ---- */

    @Override
    public void pickMedia(MediaListView list) {
        pick(REQ_MEDIA + list.kind(), list.kind() != VmSettings.KIND_CD);
    }

    @Override
    public void newDisk(MediaListView list) {
        diskCreator.ask("Novo disco (MVD)", freeDiskName(nameBase()), DEFAULT_DISK_GB,
                list::addPending, list::add);
    }

    @Override
    public void convertDisk(MediaListView list) {}

    @Override
    public void editPending(MediaListView list, int index) {
        VmSettings.Media m = list.items().get(index);
        diskCreator.ask("Disco novo", m.newName, m.newGb, (n, gb) -> {
            m.newName = n;
            m.newGb = gb;
            list.changed();
        }, uri -> {
            /* criado agora na pasta escolhida: vira um arquivo comum */
            m.uri = uri;
            m.newName = null;
            list.changed();
        });
    }

    @Override
    public void onMediaChanged(MediaListView list) {}

    /* ---- resumo ---- */

    private String mediaName(VmSettings.Media m) {
        return m.isPending() ? "novo: " + m.newName + ", até " + m.newGb + " GiB"
                : AppFiles.displayName(this, m.uri);
    }

    private void buildSummary() {
        summary.removeAllViews();
        boolean b = bios();

        addCard("Sistema", STEP_SYSTEM, R.drawable.ic_monitor,
                "Nome", vmName(),
                "Arquitetura", AppFiles.ARCH_NAMES[Math.max(0, arch.getSelectedItemPosition())],
                "Inicialização", b ? "PC com BIOS (SeaBIOS)" : "Kernel Linux direto");

        int o = Math.max(0, os.getSelectedItemPosition());
        addCard("Hardware", STEP_HARDWARE, R.drawable.ic_memory,
                "Memória", ramMb() + " MiB",
                "Sistema", OS_NAMES[o]);

        List<String> nr = new ArrayList<>();
        int sd = Math.max(0, sound.getSelectedItemPosition()), nc = Math.max(0, nic.getSelectedItemPosition());
        nr.add("Som");
        nr.add(VmSettings.SOUND_NAMES[sd] + (sd != VmSettings.SOUND_NONE && mic.isChecked() ? " + microfone" : ""));
        nr.add("Rede");
        nr.add(VmSettings.NIC_NAMES[nc]);
        for (String f : forwards) {
            nr.add("Porta");
            nr.add(VmSettings.forwardText(f));
        }
        nr.add("VNC");
        if (vnc.isChecked()) {
            int port = VmSettings.vncTcpPort(vncValue());
            nr.add((port - 5900 <= 99 ? "tela :" + (port - 5900) + " (porta " + port + ")" : "porta " + port)
                    + (vncLan.isChecked() ? ", rede local" : ", só este aparelho")
                    + (vncPassword.getText().length() > 0 ? ", com senha" : ", sem senha"));
        } else {
            nr.add("desligado");
        }
        addCard("Som e rede", STEP_NET, R.drawable.ic_network, nr.toArray(new String[0]));

        List<String> st = new ArrayList<>();
        List<VmSettings.Media> d = disks.items();
        for (int i = 0; i < d.size(); i++) {
            st.add("Disco " + (i + 1));
            st.add(mediaName(d.get(i)) + (d.get(i).ro ? " (somente leitura)" : ""));
        }
        if (d.isEmpty()) {
            st.add("Discos");
            st.add("nenhum");
        } else {
            st.add("Controlador");
            st.add(!b ? "automático (virtio)" : ctrlSata.isChecked() ? "SATA/AHCI" : ctrlVirtio.isChecked() ? "virtio" : "IDE");
        }
        addCard("Armazenamento", STEP_STORAGE, R.drawable.ic_hdd, st.toArray(new String[0]));

        List<String> bt = new ArrayList<>();
        if (b) {
            List<VmSettings.Media> c = cds.items();
            for (int i = 0; i < c.size(); i++) {
                bt.add("CD " + (i + 1));
                bt.add(mediaName(c.get(i)));
            }
            if (c.isEmpty()) {
                bt.add("CD/DVD");
                bt.add("nenhum");
            }
            List<VmSettings.Media> f = floppies.items();
            for (int i = 0; i < f.size(); i++) {
                bt.add(i == 0 ? "Disquete A:" : "Disquete B:");
                bt.add(mediaName(f.get(i)) + (f.get(i).ro ? " (protegido)" : ""));
            }
            bt.add("Ordem de boot");
            bt.add(VmSettings.BOOT_NAMES[Math.max(0, bootOrder.getSelectedItemPosition())]);
        } else {
            String cl = cmdline.getText().toString().trim();
            bt.add("Kernel");
            bt.add(kernel == null ? "(nenhum)" : AppFiles.displayName(this, kernel));
            bt.add("Initrd");
            bt.add(initrd == null ? "(nenhum)" : AppFiles.displayName(this, initrd));
            bt.add("Linha de comando");
            bt.add(cl.isEmpty() ? "padrão (" + cmdline.getHint() + ")" : cl);
            bt.add("Tela");
            bt.add(fb.isChecked() ? "framebuffer " + fbSize.getText().toString().trim() : "só terminal serial");
        }
        addCard("Inicialização", STEP_BOOT, R.drawable.ic_boot, bt.toArray(new String[0]));
    }

    /** Bloco do resumo: icone, titulo, botao "Editar" e pares rotulo/valor. */
    private void addCard(String title, int editStep, int icon, String... pairs) {
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setBackgroundResource(R.drawable.card_bg);
        card.setPadding(dp(14), dp(6), dp(6), dp(14));
        LinearLayout.LayoutParams clp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        clp.topMargin = dp(12);

        LinearLayout top = new LinearLayout(this);
        top.setGravity(Gravity.CENTER_VERTICAL);
        TextView t = new TextView(this);
        t.setText(title);
        t.setTextSize(17);
        t.setTextColor(getColor(R.color.text));
        t.setTypeface(Typeface.DEFAULT_BOLD);
        t.setCompoundDrawablesRelativeWithIntrinsicBounds(icon, 0, 0, 0);
        t.setCompoundDrawablePadding(dp(10));
        t.setCompoundDrawableTintList(android.content.res.ColorStateList.valueOf(getColor(R.color.accent)));
        top.addView(t, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        Button edit = new Button(this, null, 0, R.style.TextButton);
        edit.setText("Editar");
        edit.setTextColor(getColor(R.color.accent));
        edit.setCompoundDrawablesRelativeWithIntrinsicBounds(R.drawable.ic_edit, 0, 0, 0);
        edit.setCompoundDrawableTintList(android.content.res.ColorStateList.valueOf(getColor(R.color.accent)));
        edit.setOnClickListener(v -> showStep(editStep, true));
        top.addView(edit, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, dp(40)));
        card.addView(top);

        SpannableStringBuilder sb = new SpannableStringBuilder();
        for (int i = 0; i + 1 < pairs.length; i += 2) {
            if (sb.length() > 0) sb.append('\n');
            int st = sb.length();
            sb.append(pairs[i]).append(":  ");
            sb.setSpan(new ForegroundColorSpan(getColor(R.color.text2)), st, sb.length(), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
            sb.append(pairs[i + 1]);
        }
        TextView body = new TextView(this);
        body.setText(sb);
        body.setTextColor(getColor(R.color.text));
        body.setLineSpacing(dp(4), 1f);
        body.setPadding(0, dp(4), 0, 0);
        card.addView(body);
        summary.addView(card, clp);
    }

    /* ---- criacao ---- */

    private void create() {
        if (creating) return;
        creating = true;
        back.setEnabled(false);
        next.setEnabled(false);

        LinearLayout box = new LinearLayout(this);
        box.setGravity(Gravity.CENTER_VERTICAL);
        box.setPadding(dp(20), dp(16), dp(20), dp(16));
        box.addView(new ProgressBar(this));
        TextView msg = new TextView(this);
        msg.setText(allDisksNew() && disks.items().isEmpty() ? "Salvando a configuração…" : "Criando os discos…");
        msg.setPadding(dp(16), 0, 0, 0);
        box.addView(msg);
        AlertDialog dlg = new AlertDialog.Builder(this).setTitle("Criando a VM").setView(box)
                .setCancelable(false).show();

        final List<VmSettings.Media> pending = new ArrayList<>();
        for (VmSettings.Media m : disks.items()) if (m.isPending()) pending.add(m);
        new Thread(() -> {
            Exception error = null;
            String[] paths = new String[pending.size()];
            for (int i = 0; i < pending.size() && error == null; i++) {
                try {
                    paths[i] = AppFiles.createMvd(this, pending.get(i).newName, pending.get(i).newGb);
                } catch (Exception ex) {
                    error = ex;
                }
            }
            final Exception err = error;
            runOnUiThread(() -> {
                /* os discos criados viram arquivos comuns (uma nova tentativa nao os recria) */
                for (int i = 0; i < paths.length; i++) {
                    if (paths[i] == null) continue;
                    pending.get(i).uri = paths[i];
                    pending.get(i).newName = null;
                }
                disks.render();
                dlg.dismiss();
                creating = false;
                back.setEnabled(true);
                next.setEnabled(true);
                if (err != null) {
                    String m = err.getMessage() != null ? err.getMessage() : err.toString();
                    new AlertDialog.Builder(this).setTitle("Falha ao criar o disco").setMessage(m)
                            .setPositiveButton("OK", null).show();
                    return;
                }
                String id = saveConfig();
                leave(startNow.isChecked() ? id : null);
            });
        }, "wizard-create").start();
    }

    /** Grava a VM nova com VmSettings e devolve o id dela. */
    private String saveConfig() {
        boolean b = bios();
        VmSettings s = new VmSettings();
        s.name = vmName();
        s.arch = Math.max(0, arch.getSelectedItemPosition());
        s.ram = String.valueOf(ramMb());
        s.bios = b;
        s.cmdline = b ? "" : cmdline.getText().toString().trim();
        s.fb = !b && fb.isChecked();
        s.fbSize = fbSize.getText().toString().trim();
        s.bootOrder = b ? VmSettings.BOOT_CODES[Math.max(0, bootOrder.getSelectedItemPosition())] : "";
        s.sata = b && ctrlSata.isChecked();
        s.virtio = b && ctrlVirtio.isChecked();
        s.kernel = b ? null : kernel;
        s.initrd = b ? null : initrd;
        s.disks().addAll(disks.items());
        if (b) {
            s.cdroms().addAll(cds.items());
            s.floppies().addAll(floppies.items());
        }
        s.sound = Math.max(0, sound.getSelectedItemPosition());
        s.nic = Math.max(0, nic.getSelectedItemPosition());
        s.mic = mic.isChecked() && s.sound != VmSettings.SOUND_NONE;
        if (s.nic != VmSettings.NIC_NONE)
            s.forwards.addAll(forwards);
        s.vnc = vnc.isChecked();
        s.vncPort = Math.max(0, vncValue());
        s.vncPassword = vncPassword.getText().toString();
        s.vncLan = vncLan.isChecked();
        s.save(this);
        return s.id;
    }

    /** Volta para a lista de VMs; autostartId (se nao for null) e iniciada assim que ela abrir. */
    private void leave(String autostartId) {
        Intent it = new Intent(this, HomeActivity.class)
                .addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        if (autostartId != null) it.putExtra(HomeActivity.EXTRA_AUTOSTART, autostartId);
        startActivity(it);
        finish();
    }

    /* ---- utilitarios ---- */

    private ArrayAdapter<String> spinnerAdapter(String[] items) {
        ArrayAdapter<String> ad = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item, items);
        ad.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        return ad;
    }

    private long deviceRamMb() {
        try {
            ActivityManager am = (ActivityManager) getSystemService(ACTIVITY_SERVICE);
            ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo();
            am.getMemoryInfo(mi);
            return mi.totalMem >> 20;
        } catch (Exception e) {
            return 0;
        }
    }

    private int dp(int v) {
        return (int) (v * getResources().getDisplayMetrics().density);
    }
}
