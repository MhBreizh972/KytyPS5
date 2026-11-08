# Performance lab — protocole de mesure (Stray, PPSA02100 01.005.000)

Ces outils mesurent sans rien changer au rendu. Une branche d'optimisation n'est
retenue que si elle gagne sur **les mêmes** scènes, réglages et durées que la
référence, et sans artefact.

## 1. Environnement isolé

```
D:\KytyPS5_Optimization\
  src\              clone Git (branches perf/*), jamais l'installation de jeu
  builds\official\  build 7816ac7 (celle que tu utilises, copiée, pas déplacée)
  builds\perf\      artefact Windows de la branche testée
  runs\             CSV, captures PresentMon, run.json
```

Les sauvegardes et les fichiers du jeu restent là où ils sont. Avant la première
session, copie le dossier des sauvegardes de Stray (ne le déplace pas).

## 2. Réglages figés pour toutes les sessions

Note-les une fois dans `runs\settings.txt` et ne les change plus :

- résolution de sortie, mode de présentation (`Mailbox`/`Fifo`/`Immediate`),
  fréquence vblank ;
- `--occlusion-queries` : **activé ou non** (c'est une donnée clé, voir §5) ;
- `--shader-cache`, `--relaxed-shader-float`, `--amd-cpu`, `--readback-*` ;
- overlay de performance **fermé** pendant les mesures (il coûte du temps CPU).

Le cache de shaders doit être chaud : fais une première session de chauffe dans
chaque scène, et ne mesure qu'à partir de la deuxième.

## 3. Scènes

| Scène | Position | Mesure |
|---|---|---|
| Lente : « À l'intérieur du mur » | point fixe, caméra immobile | 60 s immobile, puis fermer l'émulateur |
| Rapide : première zone | point fixe, caméra immobile | idem |

Utilise la même sauvegarde et la même orientation de caméra à chaque fois (une
capture d'écran de référence aide à vérifier l'absence d'artefacts).

## 4. Déroulement

Dans PowerShell, depuis `D:\KytyPS5_Optimization\src` :

```powershell
# Référence instrumentée (perf/instrumentation : compteurs seulement)
.\tools\perf-lab\run-session.ps1 -Build ..\builds\instr -Label instr-wall-1 -OutDir ..\runs `
    -PresentMon C:\Tools\PresentMon.exe -Delay 90 -Seconds 60
```

Charge la sauvegarde, va au point de mesure, **ne bouge plus**. PresentMon
enregistre 60 s après le délai ; reste immobile encore quelques secondes, puis
ferme l'émulateur. Répète **3 fois** par configuration.

Matrice minimale (scène lente, puis rapide) :

| Build | Variables | But |
|---|---|---|
| officielle 7816ac7 | — (`-NoCounters`) | référence FPS sans instrumentation |
| perf/instrumentation | — | coût de l'instrumentation + diagnostic |
| perf/idle-gpu-flush | `KYTY_CP_WAKEUP=0`, `KYTY_IDLE_FLUSH_DRAWS=0` | doit égaler instrumentation |
| perf/idle-gpu-flush | `KYTY_IDLE_FLUSH_DRAWS=0` | effet du réveil des queues seul |
| perf/idle-gpu-flush | `KYTY_CP_WAKEUP=0` | effet de la soumission anticipée seule |
| perf/idle-gpu-flush | — | les deux |

`perf/idle-gpu-flush` contient les trois branches empilées, et chaque
optimisation se coupe par variable : on compare donc le même binaire.

## 5. Analyse

```powershell
python tools\perf-lab\analyze_perf_csv.py ..\runs\<run>.kyty.csv --last 55
python tools\perf-lab\analyze_perf_csv.py ..\runs\<base>.kyty.csv ..\runs\<cand>.kyty.csv --last 55
python tools\perf-lab\analyze_perf_csv.py --presentmon ..\runs\*.presentmon.csv
```

Lecture du diagnostic :

- **GPU execution ≈ durée d'image** : GPU-bound. Les gains viendront des shaders,
  des passes et des copies, pas de la synchronisation.
- **Un thread à ~100 % d'un cœur** (souvent `Thread_Gpu`) : le command processor
  est le goulot. C'est cohérent avec 17–21 % de CPU total sur 16 threads.
- **CPU waits on GPU élevé** : CPU et GPU travaillent à tour de rôle. La colonne
  détaillée indique la cause (occlusion, Finish, readback…).
- **occlusion synthetic > 0** : les queries d'occlusion sont désactivées, donc le
  jeu dessine aussi ce qu'il aurait éliminé. Compare avec `--occlusion-queries`.
- **draws/frame différents entre deux runs** : la scène n'était pas la même et la
  comparaison n'est pas valide.

Pour la chute après 30 s d'immobilité : enregistre 120 s immobile depuis
l'arrivée dans la zone, puis `--from <arrivée> --to <arrivée+120> --trend`. Les
compteurs marqués `<==` évoluent de plus de 25 % entre le début et la fin ; un
coût qui monte pendant que les FPS baissent est le premier suspect.

Un gain n'est retenu que s'il dépasse l'écart entre les 3 répétitions d'une même
configuration.

## Variables

| Variable | Défaut | Effet |
|---|---|---|
| `KYTY_PERF_CSV` | — | chemin du CSV ; active les compteurs sans l'overlay |
| `KYTY_PERF_CSV_INTERVAL_MS` | 1000 | période d'échantillonnage (min 100) |
| `KYTY_CP_WAKEUP` | 1 | 0 = comportement d'origine pour les queues suspendues |
| `KYTY_CP_BLOCKED_SPIN_US` | 50 | fenêtre de réessai avant sommeil (0–1000) |
| `KYTY_IDLE_FLUSH_DRAWS` | 8 | travaux avant soumission anticipée ; 0 = désactivé |
