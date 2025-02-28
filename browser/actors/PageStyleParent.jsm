/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
"use strict";

var EXPORTED_SYMBOLS = ["PageStyleParent"];

class PageStyleParent extends JSWindowActorParent {
  receiveMessage(msg) {
    // The top browser.
    let browser = this.browsingContext.top.embedderElement;
    if (!browser) {
      return;
    }

    let permanentKey = browser.permanentKey;
    let window = browser.ownerGlobal;
    if (window.closed) {
      return;
    }
    let styleMenu = window.gPageStyleMenu;

    switch (msg.name) {
      case "PageStyle:Add":
        if (browser.outerBrowser) {
          // We are in RDM mode and we probably
          // want to work with the outer browser.
          browser = browser.outerBrowser;
        }
        styleMenu.addBrowserStyleSheets(msg.data, permanentKey);
        break;
      case "PageStyle:Clear":
        styleMenu.clearBrowserStyleSheets(permanentKey);
        break;
    }
  }
}
