import { useCallback, useContext, useEffect, useState } from "react";
import "./App.css";
import { connect as serial_connect } from "@zmkfirmware/zmk-studio-ts-client/transport/serial";
import {
  ZMKConnection,
  ZMKCustomSubsystem,
  ZMKAppContext,
} from "@cormoran/zmk-studio-react-hook";
import {
  Request,
  Response,
  DeviceInfoResponse,
} from "./proto/zmk/device_info/device_info";

export const SUBSYSTEM_IDENTIFIER = "zmk__device_info";

function App() {
  return (
    <div className="app">
      <header className="app-header">
        <h1>ZMK Device Info</h1>
        <p>Keyboard diagnostic information</p>
      </header>

      <ZMKConnection
        renderDisconnected={({ connect, isLoading, error }) => (
          <section className="card">
            <h2>Device Connection</h2>
            {isLoading && <p>Connecting...</p>}
            {error && (
              <div className="error-message">
                <p>{error}</p>
              </div>
            )}
            {!isLoading && (
              <button
                className="btn btn-primary"
                onClick={() => connect(serial_connect)}
              >
                Connect Serial
              </button>
            )}
          </section>
        )}
        renderConnected={({ disconnect, deviceName }) => (
          <>
            <section className="card">
              <h2>Device Connection</h2>
              <div className="device-name">
                <p>Connected to: {deviceName}</p>
              </div>
              <button className="btn btn-secondary" onClick={disconnect}>
                Disconnect
              </button>
            </section>
            <DeviceInfoPanel />
          </>
        )}
      />

      <footer className="app-footer">
        <p>
          <strong>ZMK Device Info</strong> - Keyboard diagnostic information
        </p>
      </footer>
    </div>
  );
}

export function DeviceInfoPanel() {
  const zmkApp = useContext(ZMKAppContext);
  const [info, setInfo] = useState<DeviceInfoResponse | null>(null);
  const [isLoading, setIsLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const subsystem = zmkApp?.findSubsystem(SUBSYSTEM_IDENTIFIER);

  // Async fetch with no synchronous setState before the first await —
  // the caller (manual refresh button) sets the loading flag before calling this.
  const doFetch = useCallback(
    async (
      connection: typeof zmkApp.state.connection,
      subsystemIndex: number
    ) => {
      const service = new ZMKCustomSubsystem(connection!, subsystemIndex);
      const request = Request.create({ getDeviceInfo: {} });
      const payload = Request.encode(request).finish();
      const responsePayload = await service.callRPC(payload);
      if (responsePayload) {
        const resp = Response.decode(responsePayload);
        if (resp.deviceInfo) {
          setInfo(resp.deviceInfo);
        } else if (resp.error) {
          setError(resp.error.message);
        }
      }
    },
    [setInfo, setError]
  );

  // Auto-fetch when subsystem becomes available.
  useEffect(() => {
    if (!subsystem || !zmkApp?.state.connection) return;
    let cancelled = false;
    doFetch(zmkApp.state.connection, subsystem.index).catch((e) => {
      if (!cancelled)
        setError(e instanceof Error ? e.message : "Unknown error");
    });
    return () => {
      cancelled = true;
    };
  }, [subsystem?.index, doFetch, zmkApp?.state.connection]);

  const fetchInfo = async () => {
    if (!zmkApp?.state.connection || !subsystem) return;
    setIsLoading(true);
    setError(null);
    try {
      await doFetch(zmkApp.state.connection, subsystem.index);
    } catch (e) {
      setError(e instanceof Error ? e.message : "Unknown error");
    } finally {
      setIsLoading(false);
    }
  };

  if (!zmkApp) return null;

  if (!subsystem) {
    return (
      <section className="card">
        <div className="warning-message">
          <p>
            Subsystem &quot;{SUBSYSTEM_IDENTIFIER}&quot; not found. Make sure
            your firmware includes the device info module.
          </p>
        </div>
      </section>
    );
  }

  return (
    <section className="card">
      <div className="panel-header">
        <h2>Device Information</h2>
        <div className="panel-actions">
          {info && (
            <button
              className="btn btn-secondary"
              onClick={() =>
                navigator.clipboard.writeText(JSON.stringify(info, null, 2))
              }
            >
              Copy JSON
            </button>
          )}
          <button
            className="btn btn-primary"
            disabled={isLoading}
            onClick={fetchInfo}
          >
            {isLoading ? "Loading..." : "Refresh"}
          </button>
        </div>
      </div>

      {error && (
        <div className="error-message">
          <p>{error}</p>
        </div>
      )}
      {isLoading && <p>Loading...</p>}
      {info && <DeviceInfoDisplay info={info} />}
    </section>
  );
}

function DeviceInfoDisplay({ info }: { info: DeviceInfoResponse }) {
  const { build, hardware, zephyrDevices, zmkConfig, runtime } = info;

  return (
    <div className="device-info-display">
      {build && (
        <details open>
          <summary>
            <strong>Build</strong>
          </summary>
          <table className="info-table">
            <tbody>
              <InfoRow
                label="ZMK Version"
                value={build.zmkVersion}
                dirty={build.zmkDirty}
              />
              <InfoRow
                label="ZMK Config Version"
                value={build.zmkConfigVersion}
                dirty={build.zmkConfigDirty}
              />
              <InfoRow
                label="Module Version"
                value={build.moduleVersion}
                dirty={build.moduleDirty}
              />
              <InfoRow label="Zephyr Version" value={build.zephyrVersion} />
              <InfoRow label="Build Timestamp" value={build.buildTimestamp} />
              <InfoRow label="Board" value={build.board} />
            </tbody>
          </table>
        </details>
      )}

      {hardware && (
        <details open>
          <summary>
            <strong>Hardware</strong>
          </summary>
          <table className="info-table">
            <tbody>
              <InfoRow label="Device ID" value={hardware.deviceId} />
              <InfoRow
                label="Reset Cause"
                value={formatResetCause(hardware.resetCause)}
              />
              <InfoRow
                label="Flash"
                value={hardware.flashSizeKb ? `${hardware.flashSizeKb} KB` : ""}
              />
              <InfoRow
                label="SRAM"
                value={hardware.sramSizeKb ? `${hardware.sramSizeKb} KB` : ""}
              />
            </tbody>
          </table>
        </details>
      )}

      {zmkConfig && (
        <details open>
          <summary>
            <strong>ZMK Configuration</strong>
          </summary>
          <table className="info-table">
            <tbody>
              <InfoRow label="KScan" value={zmkConfig.kscanCompatible} />
              <InfoRow
                label="Split"
                value={
                  zmkConfig.splitEnabled ? zmkConfig.splitRole : "disabled"
                }
              />
              <InfoRow
                label="BLE"
                value={
                  zmkConfig.bleEnabled
                    ? `enabled (${zmkConfig.bleProfileCount} profiles)`
                    : "disabled"
                }
              />
              <InfoRow
                label="USB"
                value={zmkConfig.usbEnabled ? "enabled" : "disabled"}
              />
              <InfoRow
                label="Display"
                value={zmkConfig.displayEnabled ? "enabled" : "disabled"}
              />
              <InfoRow
                label="RGB Underglow"
                value={zmkConfig.rgbUnderglowEnabled ? "enabled" : "disabled"}
              />
              <InfoRow
                label="Backlight"
                value={zmkConfig.backlightEnabled ? "enabled" : "disabled"}
              />
              <InfoRow
                label="Battery Level"
                value={zmkConfig.batteryLevelEnabled ? "enabled" : "disabled"}
              />
            </tbody>
          </table>
        </details>
      )}

      {runtime && (
        <details open>
          <summary>
            <strong>Runtime</strong>
          </summary>
          <table className="info-table">
            <tbody>
              <InfoRow label="Uptime" value={formatUptime(runtime.uptimeMs)} />
            </tbody>
          </table>
        </details>
      )}

      {zephyrDevices && zephyrDevices.length > 0 && (
        <details>
          <summary>
            <strong>Zephyr Devices</strong>
            {zephyrDevices.filter((d) => !d.ready).length > 0 ? (
              <span className="badge-error">
                {" "}
                {zephyrDevices.filter((d) => !d.ready).length} not ready
              </span>
            ) : (
              <span className="badge-ok"> all ready</span>
            )}
          </summary>
          <div className="device-list">
            {[...zephyrDevices]
              .sort((a, b) => (a.ready === b.ready ? 0 : a.ready ? 1 : -1))
              .map((dev, idx) => (
                <div
                  key={idx}
                  className={`device-item ${dev.ready ? "device-ok" : "device-fail"}`}
                >
                  <span className="device-status">{dev.ready ? "✓" : "✗"}</span>
                  <span className="device-name">{dev.name}</span>
                </div>
              ))}
          </div>
        </details>
      )}
    </div>
  );
}

function InfoRow({
  label,
  value,
  dirty,
}: {
  label: string;
  value?: string;
  dirty?: boolean;
}) {
  if (!value) return null;
  return (
    <tr>
      <td className="info-label">{label}</td>
      <td className="info-value">
        {value}
        {dirty && <span className="badge-dirty"> (dirty)</span>}
      </td>
    </tr>
  );
}

const RESET_CAUSE_LABELS: [number, string][] = [
  [1 << 0, "External Pin"],
  [1 << 1, "Software"],
  [1 << 2, "Brownout"],
  [1 << 3, "Power-On"],
  [1 << 4, "Watchdog"],
  [1 << 5, "Debug"],
  [1 << 6, "Security"],
  [1 << 7, "Low Power Wake"],
  [1 << 8, "CPU Lockup"],
  [1 << 9, "Parity Error"],
  [1 << 10, "PLL Error"],
  [1 << 11, "Clock Error"],
  [1 << 12, "Hardware Reset"],
  [1 << 13, "User Reset"],
  [1 << 14, "Temperature"],
];

function formatResetCause(cause: number): string {
  if (!cause) return "Unknown";
  const labels = RESET_CAUSE_LABELS.filter(([bit]) => cause & bit).map(
    ([, label]) => label
  );
  return labels.length > 0 ? labels.join(", ") : "Unknown";
}

function formatUptime(ms: number | bigint): string {
  const totalMs = Number(ms);
  const s = Math.floor(totalMs / 1000);
  const m = Math.floor(s / 60);
  const h = Math.floor(m / 60);
  const d = Math.floor(h / 24);
  if (d > 0) return `${d}d ${h % 24}h ${m % 60}m`;
  if (h > 0) return `${h}h ${m % 60}m ${s % 60}s`;
  if (m > 0) return `${m}m ${s % 60}s`;
  return `${s}s`;
}

export default App;
