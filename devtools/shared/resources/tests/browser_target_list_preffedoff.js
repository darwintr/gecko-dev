/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

// Test the TargetList API when DevTools Fission preference is false

const { TargetList } = require("devtools/shared/resources/target-list");

const FISSION_TEST_URL = URL_ROOT + "/fission_document.html";

add_task(async function() {
  // Disable the preloaded process as it gets created lazily and may interfere
  // with process count assertions
  await pushPref("dom.ipc.processPrelaunch.enabled", false);
  // This preference helps destroying the content process when we close the tab
  await pushPref("dom.ipc.keepProcessesAlive.web", 1);

  const client = await createLocalClient();
  const mainRoot = client.mainRoot;
  const mainProcess = await mainRoot.getMainProcess();

  // Assert the limited behavior of this API with fission preffed off
  await pushPref("devtools.browsertoolbox.fission", false);
  await testPreffedOffMainProcess(mainRoot, mainProcess);
  await testPreffedOffTab(mainRoot);

  await client.close();
});

async function testPreffedOffMainProcess(mainRoot, mainProcess) {
  info(
    "Test TargetList when devtools's fission pref is false, via the parent process target"
  );

  const targetList = new TargetList(mainRoot, mainProcess);
  await targetList.startListening([
    TargetList.TYPES.PROCESS,
    TargetList.TYPES.FRAME,
  ]);

  // The API should only report the top level target,
  // i.e. the Main process target, which is considered as frame
  // and not as process.
  const processes = await targetList.getAllTargets(TargetList.TYPES.PROCESS);
  is(processes.length, 0);
  const frames = await targetList.getAllTargets(TargetList.TYPES.FRAME);
  is(frames.length, 1, "We get only one frame when preffed-off");
  is(
    frames[0],
    mainProcess,
    "The target is the top level one via getAllTargets"
  );

  const processTargets = [];
  const onProcessAvailable = (type, newTarget, isTopLevel) => {
    processTargets.push(newTarget);
  };
  await targetList.watchTargets([TargetList.TYPES.PROCESS], onProcessAvailable);
  is(processTargets.length, 0, "We get no process when preffed-off");
  targetList.unwatchTargets([TargetList.TYPES.PROCESS], onProcessAvailable);

  const frameTargets = [];
  const onFrameAvailable = (type, newTarget, isTopLevel) => {
    is(
      type,
      TargetList.TYPES.FRAME,
      "We are only notified about frame targets"
    );
    ok(isTopLevel, "We are only notified about the top level target");
    frameTargets.push(newTarget);
  };
  await targetList.watchTargets([TargetList.TYPES.FRAME], onFrameAvailable);
  is(
    frameTargets.length,
    1,
    "We get one frame via watchTargets when preffed-off"
  );
  is(
    frameTargets[0],
    mainProcess,
    "The target is the top level one via watchTargets"
  );
  targetList.unwatchTargets([TargetList.TYPES.FRAME], onFrameAvailable);

  targetList.stopListening([TargetList.TYPES.PROCESS, TargetList.TYPES.FRAME]);
}

async function testPreffedOffTab(mainRoot) {
  info(
    "Test TargetList when devtools's fission pref is false, via the tab target"
  );

  // Create a TargetList for a given test tab
  gBrowser.selectedTab = BrowserTestUtils.addTab(gBrowser);
  const tab = await addTab(FISSION_TEST_URL);
  const target = await mainRoot.getTab({ tab });
  const targetList = new TargetList(mainRoot, target);

  await targetList.startListening([
    TargetList.TYPES.PROCESS,
    TargetList.TYPES.FRAME,
  ]);

  const processes = await targetList.getAllTargets(TargetList.TYPES.PROCESS);
  is(processes.length, 0);
  // This only reports the top level target when devtools fission preference is off
  const frames = await targetList.getAllTargets(TargetList.TYPES.FRAME);
  is(frames.length, 1, "We get only one frame when preffed-off");
  is(frames[0], target, "The target is the top level one via getAllTargets");

  const processTargets = [];
  const onProcessAvailable = newTarget => {
    processTargets.push(newTarget);
  };
  await targetList.watchTargets([TargetList.TYPES.PROCESS], onProcessAvailable);
  is(processTargets.length, 0, "We get no process when preffed-off");
  targetList.unwatchTargets([TargetList.TYPES.PROCESS], onProcessAvailable);

  const frameTargets = [];
  const onFrameAvailable = (type, newTarget, isTopLevel) => {
    is(
      type,
      TargetList.TYPES.FRAME,
      "We are only notified about frame targets"
    );
    ok(isTopLevel, "We are only notified about the top level target");
    frameTargets.push(newTarget);
  };
  await targetList.watchTargets([TargetList.TYPES.FRAME], onFrameAvailable);
  is(
    frameTargets.length,
    1,
    "We get one frame via watchTargets when preffed-off"
  );
  is(
    frameTargets[0],
    target,
    "The target is the top level one via watchTargets"
  );
  targetList.unwatchTargets([TargetList.TYPES.FRAME], onFrameAvailable);

  targetList.stopListening([TargetList.TYPES.PROCESS, TargetList.TYPES.FRAME]);

  BrowserTestUtils.removeTab(tab);
}
