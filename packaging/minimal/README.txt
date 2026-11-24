MiSTerPlex v1.0.2 (build <build>)
==================================

This is the minimal MiSTerPlex package: the FPGA core, the ARM program and its
boot scripts. It installs or updates those files on a MiSTer. Full guide:
https://github.com/flynnsbit/misterplex-cast/blob/main/docs/INSTALL.md

Files
  MiSTerPlex.rbf           FPGA core      -> /media/fat/_Utility/MiSTerPlex.rbf
  misterplexd              ARM program    -> /media/fat/misterplex/bin/misterplexd
                                             and /media/fat/misterplex/bin/misterplexd-spelunkr
  owenb_idle_card          idle screen    -> /media/fat/misterplex/bin/
  misterplexd-boot-rc3.sh  boot script    -> /media/fat/misterplex/bin/
  misterplex_mode.sh       mode switcher  -> /media/fat/misterplex/bin/
  install-misterplex.sh    installer      (stays in this folder)
  MD5SUMS                  checksums      (stays in this folder)

Not in this package: the settings example, Scripts/configure-misterplex.sh, the
uninstall and update_ytdlp.sh scripts, the restart supervisor, the AirPlay
helper and its ffmpeg, yt-dlp, the CA bundle and the menu fonts. An existing
MiSTerPlex install keeps the copies it already has; a new card needs a full
package for AirPlay, YouTube and automatic restart.

Install or update (on the MiSTer, as root)
  1. Copy this folder to /tmp on the MiSTer (it is gone after the next reboot,
     so nothing is left on the SD card):
       scp -r misterplex-<build> root@<mister-ip>:/tmp/
  2. ssh root@<mister-ip>
     cd /tmp/misterplex-<build> && sh install-misterplex.sh
     The installer checks every file against MD5SUMS first; on a mismatch it
     installs nothing. Then it copies the new files (the core to
     /media/fat/_Utility/MiSTerPlex.rbf only), moves an old different core
     at /media/fat/MiSTerPlex.rbf aside (the boot script would load it
     first), and adds the start-at-boot line to
     /media/fat/linux/user-startup.sh if it is not there yet (never twice).
     Files it replaces are saved to /media/fat/misterplex/backup-<time>/
     with a restore.sh that undoes the install. Old PlexOB.rbf files are
     saved there on every run (about 4 MB each), so a re-run makes a new
     backup folder when you have them; otherwise, if nothing changes, no
     backup folder is made. Add --dry-run to see what it would do.
  3. Reboot. The boot script loads MiSTerPlex by itself when the MiSTer comes
     up on its menu; you do not need to pick it from _Utility. If you later
     open another core, load MiSTerPlex from _Utility to come back.
  4. First time only: link the MiSTer at https://plex.tv/link with the code
     shown on the Ready screen.

If the installer can't
  If a step fails, the installer stops with "FAILED at step: ..." and prints
  the command that undoes everything it changed so far:
    sh /media/fat/misterplex/backup-<time>/restore.sh
  Run it and reboot, or fix the cause and run the installer again. Only if it
  keeps failing, do the two setup steps by hand, then reboot:
    sh /media/fat/misterplex/bin/misterplexd-boot-rc3.sh install
    mv /media/fat/MiSTerPlex.rbf /media/fat/misterplex/backup-<time>/

Settings
  Your settings file, /media/fat/linux/misterplex.conf, is never changed or
  replaced by this installer. On a new install there is no settings file yet
  and MiSTerPlex runs with its built-in defaults. To start from the example
  file, download it on your computer and copy it over, only if the MiSTer
  has no settings file yet:
    https://raw.githubusercontent.com/flynnsbit/misterplex-cast/v1.0.2/assets/misterplex.conf.example
    ssh root@<mister-ip> test -e /media/fat/linux/misterplex.conf \
      || scp misterplex.conf.example root@<mister-ip>:/media/fat/linux/misterplex.conf
  Reboot after a change.

Clean up
  If you copied this package folder to /media/fat instead of /tmp, delete it.
  An old core named PlexOB.rbf (from before the rename) is usually left in
  place; the boot script prefers _Utility/MiSTerPlex.rbf, and you can delete
  it. If an earlier full install left misterplex/cores/MiSTerPlex.rbf.md5,
  the boot script may remove old PlexOB.rbf files (now or at a later boot or
  mode switch); the installer saves every one it finds to the backup folder
  first, on every run, and restore.sh puts back any that are gone.

Undo
  sh /media/fat/misterplex/backup-<time>/restore.sh   then reboot.
  It stops MiSTerPlex, puts user-startup.sh back as it was (so a boot line
  the installer added is gone), puts the saved files back and deletes the
  files the install created. It also works after a failed install. To stop
  MiSTerPlex starting at boot without undoing the install, run:
    sh /media/fat/misterplex/bin/misterplexd-boot-rc3.sh uninstall

Source
  The program reports build <build>. The public source for this build is tag
  v1.0.2 of https://github.com/flynnsbit/misterplex-cast (a single-commit
  source export of that build, so the tag shows its own commit id) and
  misterplex-source-<build>.tar.gz on the release page. Licences are in the
  source and on the release page; they are not copied to the MiSTer.
