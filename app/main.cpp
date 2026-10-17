#include "../inc/Server.hpp"
#include <exception>
#include <iostream>
#include <csignal>

// Config que se usa si se ejecuta ./webserv sin argumento
#define DEFAULT_CONFIG_PATH "etc/default.conf"

// Ctrl+C (SIGINT) o SIGTERM: solo levantamos la bandera. El bucle de eventos
// la ve, sale y cierra clientes, CGIs y sockets de forma ordenada.
static void onShutdownSignal(int)
{
    g_shutdown_requested = 1;
}

int main(int argc, char** argv)
{
    if (argc > 2)
    {
        std::cerr << "Usage: " << argv[0] << " [config_file_path]" << std::endl
                  << "       (without argument, " << DEFAULT_CONFIG_PATH << " is used)" << std::endl;
        return 1;
    }

    // Si un cliente cierra la conexion mientras le enviamos datos, send()
    // generaria SIGPIPE y mataria el proceso. Lo ignoramos: send() devolvera -1
    // y cerraremos ese cliente sin mas.
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, onShutdownSignal);
    signal(SIGTERM, onShutdownSignal);

    try
    {
        Configuration config;
        // Sin argumento usamos etc/default.conf. El subject lo permite
        // (IV.1 Requirements): "Your program must use a configuration file,
        // provided as an argument on the command line, or available in a
        // default path."
        const char* config_path = argc == 2 ? argv[1] : DEFAULT_CONFIG_PATH;
        if (argc == 1)
            std::cout << "\n  " << YELLOW << "No config file given: using the default "
                      << DEFAULT_CONFIG_PATH << NC << "\n  " << GRAY
                      << "(subject, IV.1: \"a configuration file, provided as an argument"
                      << " on the command line, or available in a default path\")" << NC
                      << std::endl;
        Configuration::parse(config_path, config);
        config.print(config_path);
        Server server(config);
        // std::cout << "Inicializando sockets..." << std::endl;
        server.setupListeningSockets();
        std::cout << "\n  " << GREEN << BOLD << "Ready" << NC << GRAY
                  << "  ·  press Ctrl+C to stop" << NC << "\n" << std::endl;
        server.run();
    }
    catch (const std::exception& error)
    {
        std::cerr << "webserv: " << error.what() << std::endl;
        return 1;
    }
    catch (...)
    {
        std::cerr << "webserv: unexpected fatal error" << std::endl;
        return 1;
    }

    return 0;
}
