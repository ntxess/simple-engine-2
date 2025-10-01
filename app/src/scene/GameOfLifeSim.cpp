#include "GameOfLifeSim.hpp"

GameOfLifeSim::GameOfLifeSim()
    : m_appContext(nullptr)
    , m_width()
    , m_height()
{
    m_grids[0].resize(1920 * 1080, 0);
    m_grids[1].resize(1920 * 1080, 0);
}

GameOfLifeSim::GameOfLifeSim(ApplicationContext* sysData)
    : m_appContext(sysData)
    , m_width(m_appContext->configData.get<int>("width").value_or(1920))
    , m_height(m_appContext->configData.get<int>("height").value_or(1080))
{
    m_grids[0].resize(m_width * m_height, 0);
    m_grids[1].resize(m_width * m_height, 0);
}

GameOfLifeSim::~GameOfLifeSim()
{
    m_reg.clear();
}

void GameOfLifeSim::init()
{
    const float aliveProbability = 0.07f;
    std::random_device dev;
    std::mt19937 rng(dev());
    std::bernoulli_distribution dist(aliveProbability);

    Grid& grid = m_grids[0];
    for (int y = 0; y < m_height; ++y)
    {
        for (int x = 0; x < m_width; ++x)
        {
            grid[index(x, y)] = dist(rng) ? 1 : 0;
        }
    }

    m_currentReadBuffer.store(0, std::memory_order_relaxed);
}

void GameOfLifeSim::processEvent(const sf::Event& event)
{}

void GameOfLifeSim::processInput()
{}

void GameOfLifeSim::update()
{
    int readIndex = m_currentReadBuffer.load(std::memory_order_acquire);
    int writeIndex = 1 - readIndex;

    const Grid& readGrid = m_grids[readIndex];
    Grid& writeGrid = m_grids[writeIndex];

    for (int y = 0; y < m_height; ++y)
    {
        for (int x = 0; x < m_width; ++x)
        {
            int aliveNeighbors =
                getCell(readGrid, x - 1, y - 1) + 
                getCell(readGrid, x, y - 1) + 
                getCell(readGrid, x + 1, y - 1) + 
                getCell(readGrid, x - 1, y) + 
                getCell(readGrid, x + 1, y)+ 
                getCell(readGrid, x - 1, y + 1) + 
                getCell(readGrid, x, y + 1) + 
                getCell(readGrid, x + 1, y + 1);

            uint8_t currentState = getCell(readGrid, x, y);

            if (currentState == 1)
            {
                writeGrid[index(x, y)] = (aliveNeighbors == 2 || aliveNeighbors == 3) ? 1 : 0;
            }
            else
            {
                writeGrid[index(x, y)] = (aliveNeighbors == 3) ? 1 : 0;
            }
        }
    }

    // Atomically swap
    m_currentReadBuffer.store(writeIndex, std::memory_order_release);

    //int readIndex = m_currentReadBuffer.load(std::memory_order_acquire);
    //int writeIndex = 1 - readIndex;

    //const Grid& readGrid = m_grids[readIndex];
    //Grid& writeGrid = m_grids[writeIndex];

    //const int numThreads = std::thread::hardware_concurrency(); // Usually # of cores
    //const int rowsPerThread = m_height / numThreads;

    //std::vector<std::future<void>> futures;

    //for (int t = 0; t < numThreads; ++t)
    //{
    //    int startY = t * rowsPerThread;
    //    int endY = (t == numThreads - 1) ? m_height : (t + 1) * rowsPerThread;

    //    futures.push_back(std::async(std::launch::async, [=, &readGrid, &writeGrid]()
    //        {
    //            for (int y = startY; y < endY; ++y)
    //            {
    //                for (int x = 0; x < m_width; ++x)
    //                {
    //                    int aliveNeighbors =
    //                        getCell(readGrid, x - 1, y - 1) + 
    //                        getCell(readGrid, x, y - 1) + 
    //                        getCell(readGrid, x + 1, y - 1) + 
    //                        getCell(readGrid, x - 1, y) + 
    //                        getCell(readGrid, x + 1, y)+ 
    //                        getCell(readGrid, x - 1, y + 1) + 
    //                        getCell(readGrid, x, y + 1) + 
    //                        getCell(readGrid, x + 1, y + 1);

    //                    uint8_t currentState = getCell(readGrid, x, y);

    //                    if (currentState == 1)
    //                    {
    //                        writeGrid[index(x, y)] = (aliveNeighbors == 2 || aliveNeighbors == 3) ? 1 : 0;
    //                    }
    //                    else
    //                    {
    //                        writeGrid[index(x, y)] = (aliveNeighbors == 3) ? 1 : 0;
    //                    }
    //                }
    //            }
    //        }));
    //}

    //for (auto& f : futures)
    //    f.get();

    //m_currentReadBuffer.store(writeIndex, std::memory_order_release);
}

void GameOfLifeSim::render()
{
    const auto& scrView = m_reg.view<SceneViewRenderer>();
    for (const auto& sceneTextureID : scrView)
    {
        auto& sceneRenderTexture = m_reg.get<SceneViewRenderer>(sceneTextureID);
        int readIndex = m_currentReadBuffer.load(std::memory_order_acquire);
        const Grid& readGrid = m_grids[readIndex];

        for (int y = 0; y < m_height; ++y)
        {
            for (int x = 0; x < m_width; ++x)
            {
                if (readGrid[index(x, y)] == 1)
                {
                    sf::RectangleShape rectangle;
                    rectangle.setSize({ 1, 1 });
                    rectangle.setFillColor({ 50, 168, 82 });
                    rectangle.setPosition({ static_cast<float>(x), static_cast<float>(y) });
                    sceneRenderTexture.draw(rectangle);
                }
            }
        }
    }
}

void GameOfLifeSim::pause()
{}

void GameOfLifeSim::resume()
{}

void GameOfLifeSim::setApplicationContext(ApplicationContext* context)
{
    m_appContext = context;
}

void GameOfLifeSim::accept(ISceneVisitor* visitor, entt::entity entityID)
{
    visitor->visit(this, entityID);
}

entt::registry& GameOfLifeSim::getRegistry()
{
    return m_reg;
}

const Grid& GameOfLifeSim::getCurrentGrid() const
{
    int readIndex = m_currentReadBuffer.load(std::memory_order_acquire);
    return m_grids[readIndex];
}