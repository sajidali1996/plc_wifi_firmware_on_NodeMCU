try:
    import FreeSimpleGUI as sg
except ImportError:
    import PySimpleGUI as sg
import socket
from concurrent.futures import ThreadPoolExecutor, as_completed
from pymodbus.client import ModbusTcpClient


DEFAULT_IP = "esp-plc.local"
DEFAULT_PORT = 502
RELAY_COUNT = 4  # Coils 0..3 in plc_wifi_firmware.ino


def make_client(ip: str, port: int):
    client = ModbusTcpClient(host=ip, port=port, timeout=2)
    if not client.connect():
        return None
    return client


def set_relay(client: ModbusTcpClient, relay_index: int, state: bool) -> bool:
    result = client.write_coil(address=relay_index, value=state)
    return not result.isError()


def read_relays(client: ModbusTcpClient):
    result = client.read_coils(address=0, count=RELAY_COUNT)
    if result.isError():
        return None
    return result.bits[:RELAY_COUNT]


def can_open_port(ip: str, port: int, timeout: float) -> bool:
    try:
        with socket.create_connection((ip, port), timeout=timeout):
            return True
    except OSError:
        return False


def is_valid_plc_target(host: str, port: int) -> bool:
    client = ModbusTcpClient(host=host, port=port, timeout=1)
    try:
        if not client.connect():
            return False
        result = client.read_coils(address=0, count=1)
        return not result.isError()
    except Exception:
        return False
    finally:
        client.close()


def get_local_ipv4() -> str | None:
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.connect(("8.8.8.8", 80))
        ip = sock.getsockname()[0]
        sock.close()
        return ip
    except OSError:
        try:
            ip = socket.gethostbyname(socket.gethostname())
            if ip and not ip.startswith("127."):
                return ip
        except OSError:
            return None
    return None


def discover_plc(port: int, deep_scan: bool) -> str | None:
    # Fast path: stable mDNS hostname.
    if is_valid_plc_target(DEFAULT_IP, port):
        return DEFAULT_IP

    local_ip = get_local_ipv4()
    if not local_ip:
        return None

    subnet_prefix = ".".join(local_ip.split(".")[:3])
    candidates = [f"{subnet_prefix}.{i}" for i in range(1, 255) if f"{subnet_prefix}.{i}" != local_ip]

    timeout = 0.2 if not deep_scan else 0.4
    workers = 64 if not deep_scan else 96

    open_hosts = []
    with ThreadPoolExecutor(max_workers=workers) as executor:
        futures = {executor.submit(can_open_port, ip, port, timeout): ip for ip in candidates}
        for future in as_completed(futures):
            ip = futures[future]
            try:
                if future.result():
                    open_hosts.append(ip)
            except Exception:
                pass

    for ip in open_hosts:
        if is_valid_plc_target(ip, port):
            return ip

    return None


def build_layout():
    relay_rows = []
    for i in range(RELAY_COUNT):
        relay_rows.append(
            [
                sg.Text(f"Relay {i + 1} (Coil {i})", size=(18, 1)),
                sg.Button("ON", key=f"ON_{i}", size=(7, 1)),
                sg.Button("OFF", key=f"OFF_{i}", size=(7, 1)),
                sg.Text("Unknown", key=f"STATE_{i}", size=(8, 1)),
            ]
        )

    layout = [
        [sg.Text("ESP PLC IP"), sg.Input(DEFAULT_IP, key="IP", size=(20, 1)), sg.Text("Port"), sg.Input(str(DEFAULT_PORT), key="PORT", size=(7, 1))],
        [sg.Button("Connect", key="CONNECT"), sg.Button("Disconnect", key="DISCONNECT"), sg.Button("Read Status", key="READ")],
        [sg.Button("Discover PLC", key="DISCOVER"), sg.Text("Scan"), sg.Combo(["Fast", "Deep"], default_value="Fast", readonly=True, key="SCAN_MODE", size=(8, 1))],
        [sg.Text("Discovered:"), sg.Text("-", key="FOUND", size=(30, 1))],
        *relay_rows,
        [sg.Text("Disconnected", key="STATUS", size=(50, 1), text_color="yellow")],
    ]
    return layout


def update_ui_states(window, states):
    if states is None:
        for i in range(RELAY_COUNT):
            window[f"STATE_{i}"].update("Err", text_color="red")
        return

    for i, bit in enumerate(states):
        if bit:
            window[f"STATE_{i}"].update("ON", text_color="green")
        else:
            window[f"STATE_{i}"].update("OFF", text_color="white")


def main():
    window = sg.Window("NodeMCU PLC Relay Tester", build_layout(), finalize=True)

    client = None

    while True:
        event, values = window.read()

        if event == sg.WIN_CLOSED:
            break

        if event == "CONNECT":
            ip = values["IP"].strip()
            try:
                port = int(values["PORT"])
            except ValueError:
                window["STATUS"].update("Invalid port", text_color="red")
                continue

            if client:
                client.close()
                client = None

            client = make_client(ip, port)
            if client is None:
                window["STATUS"].update(f"Failed to connect to {ip}:{port}", text_color="red")
            else:
                window["STATUS"].update(f"Connected to {ip}:{port}", text_color="green")
                states = read_relays(client)
                update_ui_states(window, states)

        elif event == "DISCONNECT":
            if client:
                client.close()
                client = None
            window["STATUS"].update("Disconnected", text_color="yellow")

        elif event == "READ":
            if not client:
                window["STATUS"].update("Not connected", text_color="red")
                continue

            states = read_relays(client)
            if states is None:
                window["STATUS"].update("Read failed", text_color="red")
            else:
                window["STATUS"].update("Status updated", text_color="green")
            update_ui_states(window, states)

        elif isinstance(event, str) and (event.startswith("ON_") or event.startswith("OFF_")):
            if not client:
                window["STATUS"].update("Not connected", text_color="red")
                continue

            relay_index = int(event.split("_")[1])
            turn_on = event.startswith("ON_")

            ok = set_relay(client, relay_index, turn_on)
            if ok:
                window["STATUS"].update(
                    f"Relay {relay_index + 1} set to {'ON' if turn_on else 'OFF'}",
                    text_color="green",
                )
                states = read_relays(client)
                update_ui_states(window, states)
            else:
                window["STATUS"].update("Write failed", text_color="red")

        elif event == "DISCOVER":
            try:
                port = int(values["PORT"])
            except ValueError:
                window["STATUS"].update("Invalid port", text_color="red")
                continue

            deep_scan = values.get("SCAN_MODE", "Fast") == "Deep"
            scan_label = "deep" if deep_scan else "fast"

            window["STATUS"].update(f"Running {scan_label} discovery...", text_color="yellow")
            window.refresh()

            found = discover_plc(port, deep_scan)
            if found is None:
                window["FOUND"].update("Not found")
                window["STATUS"].update("No Modbus PLC found on local subnet", text_color="red")
            else:
                window["FOUND"].update(found)
                window["IP"].update(found)
                window["STATUS"].update(f"PLC discovered at {found}:{port}", text_color="green")

    if client:
        client.close()
    window.close()


if __name__ == "__main__":
    main()
