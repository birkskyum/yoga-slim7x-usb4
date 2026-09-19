@echo -off
if not exist YOGA-EL2-HANDOFF.marker then
  echo STOP. Run startup.nsh first to select the correct volume.
  goto end
endif
if not exist qebspilaa64.efi then
  goto missing
endif
if not exist slbounce.efi then
  goto missing
endif
if not exist tcblaunch.exe then
  goto missing
endif
if not exist handoff.efi then
  goto missing
endif
load -nc qebspilaa64.efi
if not %lasterror% == 0 then
  echo STOP. qebspil did not load. Cold power off; do not start Linux.
  goto end
endif
load -nc slbounce.efi
if not %lasterror% == 0 then
  echo STOP. SLBounce did not load. Cold power off; do not start Linux.
  goto end
endif
echo Check the SLBounce hook-success message above before proceeding.
echo Then run handoff.efi ONCE. Do not boot any other image in this session.
goto end
:missing
echo STOP. A required file is missing. Cold power off.
:end
