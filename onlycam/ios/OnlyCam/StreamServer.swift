import Combine
import Foundation
import Network

final class StreamServer: ObservableObject {
    private var listener: NWListener?
    private let queue = DispatchQueue(label: "onlycam.http")
    private var connections: [ObjectIdentifier: Client] = [:]
    private let lock = NSLock()
    private var latestJPEG: Data?

    @Published var isListening = false
    @Published var port: UInt16 = 8080
    @Published var clientCount = 0
    @Published var lastError: String?

    private struct Client {
        let connection: NWConnection
        var streaming = false
    }

    func start(port: UInt16 = 8080) {
        stop()
        self.port = port
        do {
            let listener = try NWListener(using: .tcp, on: NWEndpoint.Port(rawValue: port)!)
            self.listener = listener
            listener.newConnectionHandler = { [weak self] connection in
                self?.accept(connection)
            }
            listener.stateUpdateHandler = { [weak self] state in
                DispatchQueue.main.async {
                    switch state {
                    case .ready:
                        self?.isListening = true
                        self?.lastError = nil
                    case .failed(let error):
                        self?.isListening = false
                        self?.lastError = error.localizedDescription
                    case .cancelled:
                        self?.isListening = false
                    default:
                        break
                    }
                }
            }
            listener.start(queue: queue)
        } catch {
            lastError = error.localizedDescription
        }
    }

    func stop() {
        lock.lock()
        connections.values.forEach { $0.connection.cancel() }
        connections.removeAll()
        lock.unlock()
        listener?.cancel()
        listener = nil
        DispatchQueue.main.async {
            self.isListening = false
            self.clientCount = 0
        }
    }

    func push(jpeg: Data) {
        lock.lock()
        latestJPEG = jpeg
        let streaming = connections.values.filter(\.streaming)
        lock.unlock()
        for client in streaming {
            sendFrame(jpeg, to: client.connection)
        }
    }

    private func accept(_ connection: NWConnection) {
        let id = ObjectIdentifier(connection)
        lock.lock()
        connections[id] = Client(connection: connection, streaming: false)
        let count = connections.count
        lock.unlock()
        DispatchQueue.main.async { self.clientCount = count }

        connection.stateUpdateHandler = { [weak self] state in
            if case .failed = state { self?.drop(id) }
            if case .cancelled = state { self?.drop(id) }
        }
        connection.start(queue: queue)
        receiveRequest(from: connection, id: id, buffer: Data())
    }

    private func drop(_ id: ObjectIdentifier) {
        lock.lock()
        connections[id]?.connection.cancel()
        connections.removeValue(forKey: id)
        let count = connections.count
        lock.unlock()
        DispatchQueue.main.async { self.clientCount = count }
    }

    private func receiveRequest(from connection: NWConnection, id: ObjectIdentifier, buffer: Data) {
        connection.receive(minimumIncompleteLength: 1, maximumLength: 64 * 1024) { [weak self] data, _, isComplete, error in
            guard let self else { return }
            if error != nil || isComplete {
                self.drop(id)
                return
            }
            var buf = buffer
            if let data { buf.append(data) }
            if let range = buf.range(of: Data("\r\n\r\n".utf8)) {
                let header = String(data: buf.subdata(in: buf.startIndex..<range.lowerBound), encoding: .utf8) ?? ""
                self.handleHTTP(header: header, connection: connection, id: id)
            } else {
                self.receiveRequest(from: connection, id: id, buffer: buf)
            }
        }
    }

    private func handleHTTP(header: String, connection: NWConnection, id: ObjectIdentifier) {
        let first = header.split(separator: "\r\n").first.map(String.init) ?? ""
        if first.contains("/stream") {
            startMJPEG(connection: connection, id: id)
            return
        }

        let body = """
        OnlyCam is running.
        Open /stream from the Windows receiver.
        """
        let response = """
        HTTP/1.1 200 OK\r
        Content-Type: text/plain; charset=utf-8\r
        Content-Length: \(body.utf8.count)\r
        Connection: close\r
        \r
        \(body)
        """
        connection.send(content: Data(response.utf8), completion: .contentProcessed { [weak self] _ in
            self?.drop(id)
        })
    }

    private func startMJPEG(connection: NWConnection, id: ObjectIdentifier) {
        let header = """
        HTTP/1.1 200 OK\r
        Cache-Control: no-cache\r
        Pragma: no-cache\r
        Connection: keep-alive\r
        Content-Type: multipart/x-mixed-replace; boundary=frame\r
        \r
        
        """
        connection.send(content: Data(header.utf8), completion: .contentProcessed { _ in })
        lock.lock()
        connections[id]?.streaming = true
        let jpeg = latestJPEG
        lock.unlock()
        if let jpeg {
            sendFrame(jpeg, to: connection)
        }
    }

    private func sendFrame(_ jpeg: Data, to connection: NWConnection) {
        var chunk = Data()
        chunk.append(contentsOf: Array("--frame\r\n".utf8))
        chunk.append(contentsOf: Array("Content-Type: image/jpeg\r\n".utf8))
        chunk.append(contentsOf: Array("Content-Length: \(jpeg.count)\r\n\r\n".utf8))
        chunk.append(jpeg)
        chunk.append(contentsOf: Array("\r\n".utf8))
        connection.send(content: chunk, completion: .contentProcessed { _ in })
    }
}
