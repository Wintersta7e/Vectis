using SampleApp.Models;

namespace SampleApp.Services
{
    public class UserService
    {
        private readonly IUserRepository _repository;

        public UserService()
        {
            _repository = new InMemoryUserRepository();
        }

        public User FindById(string id)
        {
            return _repository.FindById(id) ?? new User { Id = id, Name = "unknown" };
        }
    }

    internal class InMemoryUserRepository : IUserRepository
    {
        public User FindById(string id)
        {
            return new User { Id = id, Name = "Ada" };
        }

        public void Save(User user)
        {
            // no-op
        }
    }
}
