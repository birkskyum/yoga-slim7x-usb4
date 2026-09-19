@echo -off
for %d run (0 9)
  if exist fs%d:\YOGA-EL2-HANDOFF.marker then
    fs%d:
    cd \
    goto found
  endif
endfor
echo STOP. The handoff USB was not found. Cold power off.
goto end
:found
echo Yoga EL2 ADSP handoff checkpoint. Experimental, RAM only.
echo This is NOT a USB4 or storage test.
echo Keep LaCie, Ethernet, hubs and other data peripherals disconnected.
echo Leave only this SanDisk connected. Do not change Secure Boot or firmware settings.
echo Run handoff.nsh ONCE to load the temporary EFI drivers.
echo After BOTH loads succeed and SLBounce reports its hook, run handoff.efi ONCE.
echo Any error or unexpected reboot means STOP and cold power off. Do not retry.
:end
