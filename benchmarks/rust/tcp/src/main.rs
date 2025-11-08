use std::io;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpListener, TcpStream};
use tokio::task;

// --- Add imports for SO_REUSEPORT ---
use socket2::{Domain, Socket, Type};
use std::net::{SocketAddr, TcpListener as StdTcpListener};
// ---

/**
 * @brief Handles a single client connection.
 * This is the "fair" version with no println! calls.
 */
async fn handle_client(mut socket: TcpStream) -> io::Result<()> {
    let mut buffer = vec![0u8; 8192];
    let response = b"HTTP/1.1 200 OK\r\nContent-Length: 13\r\n\r\nHello, World!";

    loop {
        // Read from the client
        let n = socket.read(&mut buffer).await?;

        if n == 0 {
            // Connection closed
            return Ok(());
        }

        // Write response
        socket.write_all(response).await?;
    }
}

/**
 * @brief The main accept loop.
 * This function was missing, thank you for providing it!
 */
async fn accept_loop(listener: TcpListener) -> io::Result<()> {
    loop {
        // Accept connection
        let (socket, _) = listener.accept().await?;

        // Tokio's connection handling is different from io_uring.
        // It's common to set these options on the accepted socket.
        socket.set_nodelay(true)?;

        // Spawn a task to handle this client
        task::spawn(async move {
            // We don't care about the error, just drop the task
            let _ = handle_client(socket).await;
        });
    }
}

// Hardcoded for a 4-core vs 4-core test
#[tokio::main(flavor = "multi_thread", worker_threads = 8)]
async fn main() -> io::Result<()> {

    let addr: SocketAddr = "0.0.0.0:8080".parse().expect("Invalid address");

    // 1. Create a socket using socket2 to set SO_REUSEPORT
    let socket = Socket::new(Domain::for_address(addr), Type::STREAM, None)?;

    // --- This is the critical part ---
    // Set SO_REUSEPORT so all Tokio threads can accept in parallel
    #[cfg(unix)]
    if let Err(e) = socket.set_reuse_port(true) {
        eprintln!("Warning: Failed to set SO_REUSEPORT: {}. Performance may be degraded.", e);
    }
    // ---

    socket.set_reuse_address(true)?;
    socket.bind(&addr.into())?;
    socket.listen(4096)?; // Use a deep backlog

    // 2. Convert the std::net::TcpListener to a tokio::net::TcpListener
    let std_listener: StdTcpListener = socket.into();
    std_listener.set_nonblocking(true)?;
    let listener = TcpListener::from_std(std_listener)?;

    println!("Benchmark server listening on 0.0.0.0:8080");

    // 3. Spawn accept loop, it will run until the program is killed
    let accept_task = task::spawn(accept_loop(listener));

    println!("Server running with 4 workers. Press Ctrl+C to stop.");

    // 4. Wait for Ctrl+C signal for a clean shutdown
    if let Err(e) = tokio::signal::ctrl_c().await {
        eprintln!("Failed to listen for ctrl-c signal: {}", e);
    }

    println!("Shutting down server...");

    // Cancel the accept task
    accept_task.abort();

    println!("Server stopped");
    Ok(())
}