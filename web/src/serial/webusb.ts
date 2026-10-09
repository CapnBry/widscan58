// A minimal USB CDC-ACM serial port over WebUSB, for browsers that have WebUSB
// but no wired Web Serial (Chrome on most Android devices). GPL-3.0-or-later.

const CDC_CONTROL_CLASS = 2;
const CDC_DATA_CLASS = 10;
const SET_CONTROL_LINE_STATE = 0x22;
// Android's USB host stack has historically capped a bulk transfer at 16 KiB.
const MAX_TRANSFER = 16384;

/** Implements the subset of Web Serial's SerialPort that WebSerialTransport uses. */
export class UsbCdcPort {
  readable: ReadableStream<Uint8Array> | null = null;
  writable: WritableStream<Uint8Array> | null = null;
  private controlIface = 0;
  private lineState = 0; // bit 0 DTR, bit 1 RTS

  constructor(private device: USBDevice) {}

  getInfo(): SerialPortInfo {
    return { usbVendorId: this.device.vendorId, usbProductId: this.device.productId };
  }

  /** Baud rate etc. are ignored: USB Serial/JTAG has no UART behind it. */
  async open(options: SerialOptions): Promise<void> {
    const dev = this.device;
    await dev.open();
    try {
      if (!dev.configuration) await dev.selectConfiguration(1);
      const find = (cls: number) => {
        const iface = dev.configuration!.interfaces.find((i) => i.alternates[0].interfaceClass === cls);
        if (!iface) throw new Error(`no USB interface of class ${cls}`);
        return iface;
      };
      const control = find(CDC_CONTROL_CLASS);
      const data = find(CDC_DATA_CLASS);
      const endpoint = (dir: USBDirection) => {
        const ep = data.alternates[0].endpoints.find((e) => e.direction === dir && e.type === "bulk");
        if (!ep) throw new Error(`no bulk ${dir} endpoint`);
        return ep;
      };
      const epIn = endpoint("in");
      const epOut = endpoint("out");
      await dev.claimInterface(control.interfaceNumber);
      await dev.claimInterface(data.interfaceNumber);
      this.controlIface = control.interfaceNumber;
      // DTR on, RTS off: the host is present, and no reset or boot-mode strap.
      await this.setSignals({ dataTerminalReady: true, requestToSend: false });

      const size = Math.min(Math.max(options.bufferSize ?? epIn.packetSize, epIn.packetSize), MAX_TRANSFER);
      this.readable = new ReadableStream<Uint8Array>({
        pull: async (controller) => {
          for (;;) {
            const r = await dev.transferIn(epIn.endpointNumber, size);
            if (r.status !== "ok") throw new Error(`USB transfer status ${r.status}`);
            // A zero-length packet just terminates a transfer that ended on a
            // packet boundary; keep reading until there is data.
            if (r.data?.byteLength) {
              controller.enqueue(new Uint8Array(r.data.buffer, r.data.byteOffset, r.data.byteLength));
              return;
            }
          }
        },
      });
      this.writable = new WritableStream<Uint8Array>({
        write: async (chunk) => {
          const r = await dev.transferOut(epOut.endpointNumber, chunk as Uint8Array<ArrayBuffer>);
          if (r.status !== "ok") throw new Error(`USB transfer status ${r.status}`);
        },
      });
    } catch (e) {
      await dev.close().catch(() => {});
      throw e;
    }
  }

  async close(): Promise<void> {
    this.readable = this.writable = null;
    if (!this.device.opened) return;
    await this.setSignals({ dataTerminalReady: false, requestToSend: false }).catch(() => {});
    await this.device.close(); // also aborts a pending transferIn
  }

  /** USB Serial/JTAG maps DTR/RTS onto the chip's boot strap and reset, which
   *  esptool-js toggles to enter download mode. */
  async setSignals(signals: SerialOutputSignals): Promise<void> {
    if (signals.dataTerminalReady !== undefined) this.lineState = (this.lineState & ~1) | (signals.dataTerminalReady ? 1 : 0);
    if (signals.requestToSend !== undefined) this.lineState = (this.lineState & ~2) | (signals.requestToSend ? 2 : 0);
    await this.device.controlTransferOut({
      requestType: "class",
      recipient: "interface",
      request: SET_CONTROL_LINE_STATE,
      value: this.lineState,
      index: this.controlIface,
    });
  }
}

export async function requestUsbCdcPort(vendorId: number): Promise<UsbCdcPort> {
  return new UsbCdcPort(await navigator.usb.requestDevice({ filters: [{ vendorId, classCode: CDC_CONTROL_CLASS }] }));
}

export async function grantedUsbCdcPorts(): Promise<UsbCdcPort[]> {
  return (await navigator.usb.getDevices()).map((d) => new UsbCdcPort(d));
}
